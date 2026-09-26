#pragma once
#include "pch.h"
#include "Utilities/ISerializable.h"
#include "Utilities/Serializer.h"
#include "Shared/MessageManager.h"

/* This emulates a Wireless Adapters handshakes to trick the GBA into believing it is connected to one.
- Reactive, runs on GBA clock, not its own. As such it refuses any wait commands that cause the GBA to swap to its clock speed
- Properly handshakes most requests, faking radio states

The intended and tested use case is for Gen 3 Pokemon games, where stalls to check for Wireless Adapters lose time.
Connection is frame accurate and results in console accurate cycles, verified on GBI.
Startup time is estimated, and may vary from real hardware.
*/

class GbaFakeAdapter : public ISerializable
{
public:
	//Tunable timings for the adapter, not exact, in cycles. Currently only the AdapterBoot seems to matter.
	struct Timings
	{
		//SD pulse -> login answer
		uint64_t ResetReady = 0;
		//SO raised -> SI dropped
		uint64_t Ack = 0;
		//SO not raised -> SI dropped
		uint64_t AckTimeout = 0;
		//Power -> Ready
		uint64_t AdapterBoot = 83386080; //Must be at least 4.586s
	};

	//Tune Timings
	void SetTimings(Timings timings)
	{
		_timings = timings;
	}

	void PowerOn(uint64_t clock)
	{
		_powerOnClock = clock + _timings.AdapterBoot;
	}

	//Serial Data line
	void SetResetLine(bool powerHigh, uint64_t clock)
	{
		bool released = _resetLine && !powerHigh;
		_resetLine = powerHigh;

		//Adapter can't reset during bootup
		if(released && clock >= _powerOnClock) {
			Reset(clock);
		}
	}

	//Serial Output line, written by GBA, read by adapter
	void SetSO(bool high, uint64_t clock)
	{
		if(high && !_soHigh && GetSI(clock)) {
			uint64_t lowClock = clock + _timings.Ack;
			if(lowClock < _siLowClock) {
				_siLowClock = lowClock;
			}
		}
		_soHigh = high;
	}

	//Serial Input line, written by adapter, read by GBA
	bool GetSI(uint64_t clock)
	{
		return _siHigh && clock < _siLowClock;
	}

	//Simulated data transfer between the adapter and the GBA
	uint32_t Transfer(uint32_t sent, uint64_t clock)
	{
		if(_phase == AdapterState::Sleep || clock < _readyClock) {
			return 0;
		}

		uint32_t received = _shiftOut;

		switch(_phase) {
			default:
			case AdapterState::Login:
				//Only "NINTENDO" gets it out of the login.
				_loginStarted = true;
				_shiftOut = NextLoginWord(sent);
				//No acknowledge handshake during the login
				return received;

			case AdapterState::Ready:
				if(_refuseCommands) {
					//Everything answered with 0. librfu then times each request out (130 ms) and retries it twice
					//This is what causes the soft reset to take longer if performed too quickly after a hard reset
					_shiftOut = 0;
					return 0;
				}
				_shiftOut = 0x80000000;
				if((sent >> 16) == 0x9966) {
					_command = (uint8_t)sent;
					_payloadLength = (uint8_t)(sent >> 8);
					_payloadPos = 0;
					_phase = AdapterState::Receiving;
				}
				break;

			case AdapterState::Receiving:
				_payload[_payloadPos++] = sent;
				_shiftOut = 0x80000000;
				break;

			case AdapterState::Responding:
				_responsePos++;
				if(_responsePos > _responseLength) {
					_shiftOut = 0x80000000;
					_phase = _command == 0x3D && _response[0] != 0x996601EE ? AdapterState::Sleep : AdapterState::Ready;
				} else {
					_shiftOut = _response[_responsePos];
				}
				break;
		}

		if(_phase == AdapterState::Receiving && _payloadPos >= _payloadLength) {
			RunCommand();
			_responsePos = 0;
			_shiftOut = _response[0];
			_phase = AdapterState::Responding;
		}

		_siHigh = true;
		_siLowClock = clock + _timings.AckTimeout;
		return received;
	}

private:
	static constexpr uint16_t DeviceId = 0x0001; //no other adapters so static ID is fine

	//Nintendo + Device ID
	static constexpr uint32_t LoginData[5] = {
		0x494E,
		0x544E,
		0x4E45,
		0x4F44,
		0x8001,
	};

	enum class AdapterState : uint8_t
	{
		Login, //Logging in
		Ready, //Waiting for a command
		Receiving, //Receiving a command
		Responding, //Responding to a command
		Sleep, //Unresponsive till reset
	};

	//Only reachable states listed here
	enum class RadioState : uint8_t
	{
		Idle = 0,
		Hosting = 2,
		Searching = 3,
		Connecting = 4
	};

	Timings _timings = {};

	AdapterState _phase = AdapterState::Login;
	RadioState _radio = RadioState::Idle;

	bool _resetLine = false;
	bool _siHigh = false;
	bool _soHigh = false;
	bool _refuseCommands = false; //reached on login failure
	bool _loginStarted = false; //need to track login attempts to mimic failure

	uint64_t _siLowClock = 0;
	uint64_t _readyClock = 0;
	uint64_t _powerOnClock = 0;

	uint32_t _shiftOut = 0; //whats sent to the GBA

	uint8_t _loginIndex = 0; //index for login word

	uint8_t _command = 0; //command byte
	uint8_t _payloadLength = 0;
	uint8_t _payloadPos = 0;
	uint32_t _payload[256] = {};
	uint8_t _responseLength = 0;
	uint8_t _responsePos = 0;
	uint32_t _response[9] = {};

	uint32_t _radioConfig = 0;
	uint32_t _broadcast[6] = {};

	void Reset(uint64_t clock)
	{
		//login interruptions lead to command refusal
		_refuseCommands = _phase == AdapterState::Login && _loginStarted;
		_phase = AdapterState::Login;
		_radio = RadioState::Idle;
		_loginIndex = 0;
		_loginStarted = false;
		_shiftOut = 0;
		_radioConfig = 0;
		_siHigh = false;
		_readyClock = clock + _timings.ResetReady;
	}

	//Response format:
	//0x 9966 LLCC *additional words based on length
	//flip the top command bit
	void Respond(uint8_t id, uint32_t length)
	{
		_responseLength = length;
		_response[0] = 0x99660000 | length << 8 | (id | 0x80);
	}

	void AcceptCommand(uint8_t length = 0)
	{
		Respond(_command, length);
	}

	//1 is real command, 2 is fake command, shouldn't need to distinguish
	void RejectCommand(uint32_t reason = 1)
	{
		_response[1] = reason;
		Respond(0xEE, 1);
	}

	//Accept commands that don't rely on adapter clock, don't need to actually execute them
	//Sometimes the game checks the data we've been sent so it gets stored
	void RunCommand()
	{
		switch(_command) {
			case 0x10: //Hello
				AcceptCommand();
				break;

			case 0x11: //Signal level, a byte per client
				_response[1] = 0;
				AcceptCommand(1);
				break;

			case 0x12: //Version
				_response[1] = 0x00830117;
				AcceptCommand(1);
				break;

			case 0x13: //System status
				_response[1] = ((uint32_t)_radio << 24) | (_radio == RadioState::Hosting ? DeviceId : 0);
				AcceptCommand(1);
				break;

			case 0x14: //Client slot status: the next client number, then the connected clients
				_response[1] = _radio == RadioState::Hosting ? 0x00 : 0xFF;
				AcceptCommand(1);
				break;

			case 0x15: //Radio config status
				if(_radio == RadioState::Hosting) {
					for(int i = 0; i < 6; i++) {
						_response[1 + i] = _broadcast[i];
					}
					_response[7] = _radioConfig;
					_response[8] = 257;
					AcceptCommand(8);
				} else {
					for(int i = 0; i < 6; i++) {
						_response[1 + i] = 0;
					}
					_response[7] = 257;
					AcceptCommand(7);
				}
				break;

			case 0x16: //Receive broadcast data
				if(_payloadLength == 6) {
					for(int i = 0; i < 6; i++) {
						_broadcast[i] = _payload[i];
					}
				}
				AcceptCommand();
				break;

			case 0x17: //Receive radio config
				if(_payloadLength >= 1) {
					_radioConfig = _payload[0];
				}
				AcceptCommand();
				break;

			case 0x19: //Start fake host
				if(_radio == RadioState::Searching || _radio == RadioState::Connecting) {
					RejectCommand();
				} else {
					if(_radio == RadioState::Idle) {
						_radio = RadioState::Hosting;
					}
					AcceptCommand();
				}
				break;

			case 0x1A: //"Poll" connections
				if(_radio != RadioState::Hosting) {
					RejectCommand();
				} else {
					AcceptCommand();
				}
				break;

			case 0x1B: //End host: with no clients the adapter goes back to idle
				if(_radio != RadioState::Hosting) {
					RejectCommand();
				} else {
					_radio = RadioState::Idle;
					RejectCommand();
				}
				break;

			case 0x1C: //Broadcast read start
				if(_radio == RadioState::Hosting || _radio == RadioState::Connecting) {
					RejectCommand();
				} else {
					_radio = RadioState::Searching;
					AcceptCommand();
				}
				break;

			case 0x1D: //Broadcast read poll: nothing heard
				if(_radio != RadioState::Searching) {
					RejectCommand();
				} else {
					AcceptCommand();
				}
				break;

			case 0x1E: //Broadcast read end
				if(_radio != RadioState::Searching) {
					RejectCommand();
				} else {
					_radio = RadioState::Idle;
					AcceptCommand();
				}
				break;

			case 0x1F: //Connect: there is no such host, will fail
				if(_radio == RadioState::Hosting) {
					RejectCommand();
				} else {
					_radio = RadioState::Connecting;
					AcceptCommand();
				}
				break;

			case 0x20: //Check connection status
				if(_radio == RadioState::Hosting) {
					RejectCommand();
				} else {
					_response[1] = 0x01000000; //in progress
					AcceptCommand();
				}
				break;

			case 0x21: //Finish connection
				if(_radio == RadioState::Hosting) {
					RejectCommand(1);
				} else {
					_response[1] = 0x01000000; //failed
					_radio = RadioState::Idle;
					AcceptCommand(1);
				}
				break;

			case 0x24: //Send data: a host with no clients sends to nobody
				if(_radio != RadioState::Hosting) {
					RejectCommand();
				} else {
					AcceptCommand();
				}
				break;

			case 0x26: //Receive data: an empty header
				if(_radio != RadioState::Hosting) {
					RejectCommand();
				} else {
					_response[1] = 0;
					AcceptCommand(1);
				}
				break;

			case 0x25: //Send data and wait
			case 0x27: //Wait
			case 0x35:
			case 0x37: //Retransmit and wait
				//These hand the clock to the adapter, which needs a connection to wait on.
				RejectCommand();
				break;

			case 0x30: //Disconnect client
			case 0x18:
			case 0x32:
			case 0x33:
			case 0x34:
			case 0x38:
			case 0x39:
				AcceptCommand();
				break;

			case 0x3D: //Bye: low power until the next reset
				AcceptCommand();
				break;

			//shouldn't ever happen!!!
			default:
				MessageManager::Log("Adapter sent unknown command: " + std::to_string(_command));
				RejectCommand(2);
				break;
		}
	};

	//Adapter sends login words, gba responds with inverse as well
	uint32_t NextLoginWord(uint32_t received)
	{
		uint16_t high = (received >> 16);
		uint16_t low = received;

		if(_loginIndex < 4) {
			if(high == (uint16_t)~LoginData[_loginIndex] && low == LoginData[_loginIndex]) {
				_loginIndex++; //success
			}
		}

		else if(low == LoginData[4]) {
			_phase = AdapterState::Ready;
			return 0x80000000;
		}

		return ((uint32_t)LoginData[_loginIndex] << 16) | (uint16_t)~low;
	};

public:
	void Serialize(Serializer& s) override
	{
		SV(_phase);
		SV(_radio);
		SV(_shiftOut);
		SV(_loginIndex);
		SV(_loginStarted);
		SV(_command);
		SV(_payloadLength);
		SV(_payloadPos);
		SVArray(_payload, 256);
		SV(_responseLength);
		SV(_responsePos);
		SVArray(_response, 9);
		SV(_radioConfig);
		SVArray(_broadcast, 6);
		SV(_resetLine);
		SV(_siHigh);
		SV(_soHigh);
		SV(_refuseCommands);
		SV(_siLowClock);
		SV(_readyClock);
		SV(_powerOnClock);
	}
};
