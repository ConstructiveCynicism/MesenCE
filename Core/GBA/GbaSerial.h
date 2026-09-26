#pragma once
#include "pch.h"
#include "GBA/GbaTypes.h"
#include "GBA/GbaFakeAdapter.h"
#include "Shared/Emulator.h"
#include "Shared/EmuSettings.h"
#include "Shared/NotificationManager.h"
#include "Utilities/BitUtilities.h"
#include "Utilities/Serializer.h"

class GbaSerial final : public ISerializable
{
private:
	GbaSerialState _state = {};
	GbaMemoryManager* _memoryManager = nullptr;
	Emulator* _emu = nullptr;
	uint32_t _masterClockRate = 0;

	GbaFakeAdapter _fakeAdapter;
	bool _hasFakeAdapter = false;

	//Normal mode: RCNT bit 15 clear, SIOCNT bit 13 clear
	bool IsNormalMode() { return !(_state.Mode & 0x8000) && !(_state.Control & 0x2000); }
	bool IsSDHigh() { return (_state.Mode & 0xC000) == 0x8000 && (_state.Mode & 0x20) && (_state.Mode & 0x02); }

	void UpdateState()
	{
		if(_state.Active && _memoryManager->GetMasterClock() >= _state.EndMasterClock) {
			_state.Active = false;
			_state.Control &= ~0x80;

			//send the word at the clock when transfer ends
			if(_hasFakeAdapter && _state.TransferWord && IsNormalMode()) {
				uint32_t sent = _state.Data[0] | ((uint32_t)_state.Data[1] << 16);
				uint32_t received = _fakeAdapter.Transfer(sent, _state.EndMasterClock);
				_state.Data[0] = (uint16_t)received;
				_state.Data[1] = (uint16_t)(received >> 16);
			}
		}
	}

	void UpdateResetLine()
	{
		if(_hasFakeAdapter) {
			_fakeAdapter.SetResetLine(IsSDHigh(), _memoryManager->GetMasterClock());
		}
	}

	void PlugInAdapter(uint64_t clock)
	{
		GbaConfig& cfg = _emu->GetSettings()->GetGbaConfig();
		GbaFakeAdapter::Timings timings;
		timings.ResetReady = cfg.FakeAdapterResetReady;
		timings.Ack = cfg.FakeAdapterAck;
		timings.AckTimeout = cfg.FakeAdapterAckTimeout;
		timings.AdapterBoot = cfg.FakeAdapterAdapterBoot;

		_fakeAdapter.SetTimings(timings);
		_fakeAdapter.PowerOn(clock);
		_fakeAdapter.SetResetLine(IsSDHigh(), clock);
		if(IsNormalMode()) {
			_fakeAdapter.SetSO(_state.Control & 0x08, clock);
		}
	}

public:
	//note: masterClockRate doesn't get updated until ROM load
	void Init(Emulator* emu, GbaMemoryManager* memoryManager, uint32_t masterClockRate)
	{
		_emu = emu;
		_masterClockRate = masterClockRate;
		_memoryManager = memoryManager;
		_state.IrqMasterClock = UINT64_MAX;

		if(emu->GetSettings()->GetGbaConfig().SkipBootScreen) {
			//BIOS leaves serial registers in this state, some games expect this
			_state.Mode = 0x8000;
		}
	}

	//Called at start to avoid landing midframe
	void ApplyAdapter()
	{
		bool shouldHaveAdapter = _emu->GetSettings()->GetGbaConfig().FakeAdapter;
		if(shouldHaveAdapter == _hasFakeAdapter) {
			return;
		}

		UpdateState();
		_hasFakeAdapter = shouldHaveAdapter;
		if(_hasFakeAdapter) {
			PlugInAdapter(_memoryManager->GetMasterClock());
		}
	}

	__forceinline bool HasPendingIrq()
	{
		return _state.IrqMasterClock != UINT64_MAX;
	}

	void CheckForIrq(uint64_t masterClock)
	{
		if(masterClock >= _state.IrqMasterClock) {
			if(_hasFakeAdapter) {
				//Finish transfer so answer gets read
				UpdateState();
			}
			_memoryManager->SetIrqSource(GbaIrqSource::Serial);
			_state.IrqMasterClock = UINT64_MAX;
		}
	}

	uint8_t ReadRegister(uint32_t addr, bool peek)
	{
		//TODOGBA - serial support
		if(_hasFakeAdapter && !peek) {
			UpdateState(); //Finish transfers before observation
		}

		switch(addr) {
			case 0x120:
			case 0x122:
			case 0x124:
			case 0x126:
				return BitUtilities::GetBits<0>(_state.Data[(addr & 0x06) >> 1]);

			case 0x121:
			case 0x123:
			case 0x125:
			case 0x127:
				return BitUtilities::GetBits<8>(_state.Data[(addr & 0x06) >> 1]);

			case 0x128:
				if(peek) {
					uint8_t control = _state.Control;
					if(_state.Active && _memoryManager->GetMasterClock() >= _state.EndMasterClock) {
						control &= ~0x80;
					}
					return ApplySI(control);
				} else {
					UpdateState();
					return ApplySI(BitUtilities::GetBits<0>(_state.Control));
				}

			case 0x129: return BitUtilities::GetBits<8>(_state.Control);

			case 0x12A: return BitUtilities::GetBits<0>(_state.SendData);
			case 0x12B: return BitUtilities::GetBits<8>(_state.SendData);

			case 0x134: return BitUtilities::GetBits<0>(_state.Mode);
			case 0x135: return BitUtilities::GetBits<8>(_state.Mode);
			case 0x136: return 0;
			case 0x137: return 0;

			case 0x140: return BitUtilities::GetBits<0>(_state.JoyControl);
			case 0x141: return BitUtilities::GetBits<8>(_state.JoyControl);
			case 0x142: return 0;
			case 0x143: return 0;

			case 0x150: return BitUtilities::GetBits<0>(_state.JoyReceive);
			case 0x151: return BitUtilities::GetBits<8>(_state.JoyReceive);
			case 0x152: return BitUtilities::GetBits<16>(_state.JoyReceive);
			case 0x153: return BitUtilities::GetBits<24>(_state.JoyReceive);

			case 0x154: return BitUtilities::GetBits<0>(_state.JoySend);
			case 0x155: return BitUtilities::GetBits<8>(_state.JoySend);
			case 0x156: return BitUtilities::GetBits<16>(_state.JoySend);
			case 0x157: return BitUtilities::GetBits<24>(_state.JoySend);

			case 0x158: return _state.JoyStatus;
		}

		return _memoryManager->GetOpenBus(addr);
	}

	//SIOCNT bit 2 is the SI line, which is driven with adapter acknowledgement
	uint8_t ApplySI(uint8_t control)
	{
		if(_hasFakeAdapter) {
			control = (control & ~0x04) | (_fakeAdapter.GetSI(_memoryManager->GetMasterClock()) ? 0x04 : 0);
		}
		return control;
	}
	void WriteRegister(uint32_t addr, uint8_t value)
	{
		//TODOGBA - serial support
		if(_hasFakeAdapter) {
			//Finish any ended transfer before adapter observes, or before registers overwritten
			UpdateState();
		}

		switch(addr) {
			case 0x120:
			case 0x122:
			case 0x124:
			case 0x126:
				BitUtilities::SetBits<0>(_state.Data[(addr & 0x06) >> 1], value);
				break;

			case 0x121:
			case 0x123:
			case 0x125:
			case 0x127:
				BitUtilities::SetBits<8>(_state.Data[(addr & 0x06) >> 1], value);
				break;

			case 0x128: {
				UpdateState();

				BitUtilities::SetBits<0>(_state.Control, value);

				_state.InternalShiftClock = value & 0x01;
				_state.InternalShiftClockSpeed2MHz = value & 0x02;

				if(_hasFakeAdapter && IsNormalMode()) {
					//Normal Mode has SO set as bit 3
					_fakeAdapter.SetSO(value & 0x08, _memoryManager->GetMasterClock());
				}

				bool active = value & 0x80;
				//if for some reason we end up waiting on adapter clock, this tracks it
				bool waitingForClock = _state.Active && _state.EndMasterClock == UINT64_MAX;
				if(active && _hasFakeAdapter && !_state.InternalShiftClock) {
					//if clock is set to the adapter on port, the adapter fails to set the clock so transfer waits
					if(!_state.Active) {
						_state.StartMasterClock = _memoryManager->GetMasterClock();
						_state.EndMasterClock = UINT64_MAX;
						_state.IrqMasterClock = UINT64_MAX;
					}
				} else if(active && (!_state.Active || waitingForClock)) {
					_state.StartMasterClock = _memoryManager->GetMasterClock();
					_state.EndMasterClock = _state.StartMasterClock + (_state.InternalShiftClockSpeed2MHz ? 8 : 64) * (_state.TransferWord ? 32 : 8);
					_state.EndMasterClock += 6;
					if(_state.IrqEnabled) {
						_state.IrqMasterClock = _state.EndMasterClock;
						_memoryManager->SetPendingUpdateFlag();
					}
				}
				_state.Active = active;
				break;
			}
			case 0x129:
				UpdateState();

				BitUtilities::SetBits<8>(_state.Control, value);

				_state.TransferWord = value & 0x10;
				_state.IrqEnabled = value & 0x40;
				if(!_state.IrqEnabled) {
					_state.IrqMasterClock = UINT64_MAX;
				}

				if(_state.Active) {
					if(_state.StartMasterClock == _memoryManager->GetMasterClock() && _state.EndMasterClock != UINT64_MAX) {
						//Update end based on params
						_state.EndMasterClock = _state.StartMasterClock + (_state.InternalShiftClockSpeed2MHz ? 8 : 64) * (_state.TransferWord ? 32 : 8);
						_state.EndMasterClock += 6;
					}
					if(_state.IrqEnabled) {
						_state.IrqMasterClock = _state.EndMasterClock;
						_memoryManager->SetPendingUpdateFlag();
					}
				}
				break;

			case 0x12A: BitUtilities::SetBits<0>(_state.SendData, value); break;
			case 0x12B: BitUtilities::SetBits<8>(_state.SendData, value); break;

			case 0x134:
				BitUtilities::SetBits<0>(_state.Mode, value);
				UpdateResetLine();
				break;
			case 0x135:
				BitUtilities::SetBits<8>(_state.Mode, value & 0xC1);
				UpdateResetLine();
				break;

			case 0x140: BitUtilities::SetBits<0>(_state.JoyControl, value); break;
			case 0x141: BitUtilities::SetBits<8>(_state.JoyControl, value); break;

			case 0x150: BitUtilities::SetBits<0>(_state.JoyReceive, value); break;
			case 0x151: BitUtilities::SetBits<8>(_state.JoyReceive, value); break;
			case 0x152: BitUtilities::SetBits<16>(_state.JoyReceive, value); break;
			case 0x153: BitUtilities::SetBits<24>(_state.JoyReceive, value); break;

			case 0x154: BitUtilities::SetBits<0>(_state.JoySend, value); break;
			case 0x155: BitUtilities::SetBits<8>(_state.JoySend, value); break;
			case 0x156: BitUtilities::SetBits<16>(_state.JoySend, value); break;
			case 0x157: BitUtilities::SetBits<24>(_state.JoySend, value); break;

			case 0x158: _state.JoyStatus = value; break;
		}
	}

	void Serialize(Serializer& s) override
	{
		SV(_state.Control);
		SVArray(_state.Data, 4);

		SV(_state.SendData);
		SV(_state.Mode);
		SV(_state.JoyControl);
		SV(_state.JoyReceive);
		SV(_state.JoySend);
		SV(_state.JoyStatus);

		if(s.GetFormat() != SerializeFormat::Map) {
			SV(_state.StartMasterClock);
			SV(_state.EndMasterClock);
			SV(_state.IrqMasterClock);

			SV(_state.InternalShiftClock);
			SV(_state.InternalShiftClockSpeed2MHz);
			SV(_state.Active);
			SV(_state.TransferWord);
			SV(_state.IrqEnabled);

			SV(_hasFakeAdapter);
			if(_hasFakeAdapter) {
				SV(_fakeAdapter);
			}

			if(!s.IsSaving()) {
				GbaConfig& cfg = _emu->GetSettings()->GetGbaConfig();
				if(cfg.FakeAdapter != _hasFakeAdapter) {
					cfg.FakeAdapter = _hasFakeAdapter;
					//Let the UI update its copy of the setting
					_emu->GetNotificationManager()->SendNotification(ConsoleNotificationType::RequestConfigChange);
				}
			}
		}
	}
};