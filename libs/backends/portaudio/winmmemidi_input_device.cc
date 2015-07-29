/*
 * Copyright (C) 2015 Tim Mayberry <mojofunk@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include "winmmemidi_input_device.h"

#include <stdexcept>
#include <cmath>

#include "pbd/compose.h"

#include "win_utils.h"
#include "midi_util.h"

#include "debug.h"

static const uint32_t MIDI_BUFFER_SIZE = 32768;
static const uint32_t SYSEX_BUFFER_SIZE = 32768;

namespace ARDOUR {

WinMMEMidiInputDevice::WinMMEMidiInputDevice (int index)
	: m_handle(0)
	, m_midi_buffer(new RingBuffer<uint8_t>(MIDI_BUFFER_SIZE))
	, m_sysex_buffer(new uint8_t[SYSEX_BUFFER_SIZE])
{
	DEBUG_MIDI (string_compose ("Creating midi input device index: %1\n", index));

	std::string error_msg;

	if (!open (index, error_msg)) {
		DEBUG_MIDI (error_msg);
		throw std::runtime_error (error_msg);
	}

	// perhaps this should be called in open
	if (!add_sysex_buffer (error_msg)) {
		DEBUG_MIDI (error_msg);
		std::string close_error;
		if (!close (close_error)) {
			DEBUG_MIDI (close_error);
		}
		throw std::runtime_error (error_msg);
	}

	set_device_name (index);
}

WinMMEMidiInputDevice::~WinMMEMidiInputDevice ()
{
	std::string error_msg;
	if (!close (error_msg)) {
		DEBUG_MIDI (error_msg);
	}
}

bool
WinMMEMidiInputDevice::open (UINT index, std::string& error_msg)
{
	MMRESULT result = midiInOpen (&m_handle,
	                              index,
	                              (DWORD_PTR) winmm_input_callback,
	                              (DWORD_PTR) this,
	                              CALLBACK_FUNCTION | MIDI_IO_STATUS);
	if (result != MMSYSERR_NOERROR) {
		error_msg = get_error_string (result);
		return false;
	}
	DEBUG_MIDI (string_compose ("Opened MIDI device index %1\n", index));
	return true;
}

bool
WinMMEMidiInputDevice::close (std::string& error_msg)
{
	// return error message for first error encountered?
	bool success = true;

	MMRESULT result = midiInReset (m_handle);
	if (result != MMSYSERR_NOERROR) {
		error_msg = get_error_string (result);
		DEBUG_MIDI (error_msg);
		success = false;
	}
	result = midiInUnprepareHeader (m_handle, &m_sysex_header, sizeof(MIDIHDR));
	if (result != MMSYSERR_NOERROR) {
		error_msg = get_error_string (result);
		DEBUG_MIDI (error_msg);
		success = false;
	}
	result = midiInClose (m_handle);
	if (result != MMSYSERR_NOERROR) {
		error_msg = get_error_string (result);
		DEBUG_MIDI (error_msg);
		success = false;
	}
	m_handle = 0;
	if (success) {
		DEBUG_MIDI (string_compose ("Closed MIDI device: %1\n", name ()));
	} else {
		DEBUG_MIDI (string_compose ("Unable to Close MIDI device: %1\n", name ()));
	}
	return success;
}

bool
WinMMEMidiInputDevice::add_sysex_buffer (std::string& error_msg)
{
	m_sysex_header.dwBufferLength = SYSEX_BUFFER_SIZE;
	m_sysex_header.dwFlags = 0;
	m_sysex_header.lpData = (LPSTR)m_sysex_buffer.get ();

	MMRESULT result = midiInPrepareHeader (m_handle, &m_sysex_header, sizeof(MIDIHDR));

	if (result != MMSYSERR_NOERROR) {
		error_msg = get_error_string (result);
		DEBUG_MIDI (error_msg);
		return false;
	}
	result = midiInAddBuffer (m_handle, &m_sysex_header, sizeof(MIDIHDR));
	if (result != MMSYSERR_NOERROR) {
		error_msg = get_error_string (result);
		DEBUG_MIDI (error_msg);
		return false;
	}
	return true;
}

bool
WinMMEMidiInputDevice::set_device_name (UINT index)
{
	MIDIINCAPS capabilities;
	MMRESULT result = midiInGetDevCaps (index, &capabilities, sizeof(capabilities));
	if (result != MMSYSERR_NOERROR) {
		DEBUG_MIDI (get_error_string (result));
		m_name = "Unknown Midi Input Device";
		return false;
	} else {
		m_name = capabilities.szPname;
	}
	return true;
}

std::string
WinMMEMidiInputDevice::get_error_string (MMRESULT error_code)
{
	char error_msg[MAXERRORLENGTH];
	MMRESULT result = midiInGetErrorText (error_code, error_msg, MAXERRORLENGTH);
	if (result != MMSYSERR_NOERROR) {
		return error_msg;
	}
	return "WinMMEMidiInput: Unknown Error code";
}

void CALLBACK
WinMMEMidiInputDevice::winmm_input_callback(HMIDIIN handle,
                                            UINT msg,
                                            DWORD_PTR instance,
                                            DWORD_PTR midi_msg,
                                            DWORD timestamp)
{
	WinMMEMidiInputDevice* midi_input = (WinMMEMidiInputDevice*)instance;

	switch (msg) {
	case MIM_OPEN:
	case MIM_CLOSE:
		// devices_changed_callback
		break;
	case MIM_MOREDATA:
		// passing MIDI_IO_STATUS to midiInOpen means that MIM_MOREDATA
		// will be sent when the callback isn't processing MIM_DATA messages
		// fast enough to keep up with messages arriving at input device
		// driver. I'm not sure what could be done differently if that occurs
		// so just handle MIM_DATA as per normal
	case MIM_DATA:
		midi_input->handle_short_msg ((const uint8_t*)&midi_msg, (uint32_t)timestamp);
		break;
	case MIM_LONGDATA:
		midi_input->handle_sysex_msg ((MIDIHDR*)&midi_msg, (uint32_t)timestamp);
		break;
	case MIM_ERROR:
		DEBUG_MIDI ("WinMME: Driver sent an invalid MIDI message\n");
		break;
	case MIM_LONGERROR:
		DEBUG_MIDI ("WinMME: Driver sent an invalid or incomplete SYSEX message\n");
		break;
	}
}

void
WinMMEMidiInputDevice::handle_short_msg (const uint8_t* midi_data,
                                         uint32_t timestamp)
{
	int length = get_midi_msg_length (midi_data[0]);

	if (length == 0 || length == -1) {
		DEBUG_MIDI ("ERROR: midi input driver sent an invalid midi message\n");
		return;
	}

	enqueue_midi_msg (midi_data, length, timestamp);
}

void
WinMMEMidiInputDevice::handle_sysex_msg (MIDIHDR* const midi_header,
                                         uint32_t timestamp)
{
#ifdef ENABLE_SYSEX
	LPMIDIHDR header = (LPMIDIHDR)midi_header;
	size_t byte_count = header->dwBytesRecorded;

	if (!byte_count) {
		DEBUG_MIDI (
		    "ERROR: WinMME driver has returned sysex header to us with no bytes\n");
		return;
	}

	uint8_t* data = (uint8_t*)header->lpData;

	if ((data[0] != 0xf0) || (data[byte_count - 1] != 0xf7)) {
		DEBUG_MIDI (string_compose ("Discarding %1 byte sysex chunk\n", byte_count));
	} else {
		enqueue_midi_msg (data, byte_count, timestamp);
	}

	MMRESULT result = midiInAddBuffer (m_handle, &m_sysex_header, sizeof(MIDIHDR));
	if (result != MMSYSERR_NOERROR) {
		DEBUG_MIDI (get_error_string (result));
	}
#endif
}

// fix param order
bool
WinMMEMidiInputDevice::dequeue_midi_event (uint64_t timestamp_start,
                                           uint64_t timestamp_end,
                                           uint64_t& timestamp,
                                           uint8_t* midi_data,
                                           size_t& data_size)
{
	const uint32_t read_space = m_midi_buffer->read_space();
	struct MidiEventHeader h(0,0);

	if (read_space <= sizeof(MidiEventHeader)) {
		return false;
	}

	RingBuffer<uint8_t>::rw_vector vector;
	m_midi_buffer->get_read_vector (&vector);
	if (vector.len[0] >= sizeof(MidiEventHeader)) {
		memcpy ((uint8_t*)&h, vector.buf[0], sizeof(MidiEventHeader));
	} else {
		if (vector.len[0] > 0) {
			memcpy ((uint8_t*)&h, vector.buf[0], vector.len[0]);
		}
		assert (vector.buf[1] || vector.len[0] == sizeof(MidiEventHeader));
		memcpy (((uint8_t*)&h) + vector.len[0],
		        vector.buf[1],
		        sizeof(MidiEventHeader) - vector.len[0]);
	}

	if (h.time >= timestamp_end) {
		DEBUG_TIMING (string_compose ("WinMMEMidiInput EVENT %1(ms) early\n",
		                              (h.time - timestamp_end) * 1e-3));
		return false;
	} else if (h.time < timestamp_start) {
		DEBUG_TIMING (string_compose ("WinMMEMidiInput EVENT %1(ms) late\n",
		                              (timestamp_start - h.time) * 1e-3));
	}

	m_midi_buffer->increment_read_idx (sizeof(MidiEventHeader));

	assert (h.size > 0);
	if (h.size > data_size) {
		DEBUG_MIDI ("WinMMEMidiInput::dequeue_event MIDI event too large!\n");
		m_midi_buffer->increment_read_idx (h.size);
		return false;
	}
	if (m_midi_buffer->read (&midi_data[0], h.size) != h.size) {
		DEBUG_MIDI ("WinMMEMidiInput::dequeue_event Garbled MIDI EVENT DATA!!\n");
		return false;
	}
	timestamp = h.time;
	data_size = h.size;
	return true;
}

bool
WinMMEMidiInputDevice::enqueue_midi_msg (const uint8_t* midi_data,
                                         size_t data_size,
                                         uint32_t timestamp)
{
	const uint32_t total_size = sizeof(MidiEventHeader) + data_size;

	if (data_size == 0) {
		DEBUG_MIDI ("ERROR: zero length midi data\n");
		return false;
	}

	if (m_midi_buffer->write_space () < total_size) {
		DEBUG_MIDI ("WinMMEMidiInput: ring buffer overflow\n");
		return false;
	}

	// don't use winmme timestamps for now
	uint64_t ts = utils::get_microseconds ();

	DEBUG_TIMING (string_compose (
	    "Enqueing MIDI data device: %1 with timestamp: %2 and size %3\n",
	    name (),
	    ts,
	    data_size));

	struct MidiEventHeader h (ts, data_size);
	m_midi_buffer->write ((uint8_t*)&h, sizeof(MidiEventHeader));
	m_midi_buffer->write (midi_data, data_size);
	return true;
}

bool
WinMMEMidiInputDevice::start ()
{
	if (!m_started) {
		MMRESULT result = midiInStart (m_handle);
		m_started = (result == MMSYSERR_NOERROR);
		if (!m_started) {
			DEBUG_MIDI (get_error_string (result));
		} else {
			DEBUG_MIDI (
			    string_compose ("WinMMEMidiInput: device %1 started\n", name ()));
		}
	}
	return m_started;
}

bool
WinMMEMidiInputDevice::stop ()
{
	if (m_started) {
		MMRESULT result = midiInStop (m_handle);
		m_started = (result != MMSYSERR_NOERROR);
		if (m_started) {
			DEBUG_MIDI (get_error_string (result));
		} else {
			DEBUG_MIDI (
			    string_compose ("WinMMEMidiInput: device %1 stopped\n", name ()));
		}
	}
	return !m_started;
}

} // namespace ARDOUR