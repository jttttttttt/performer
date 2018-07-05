#include "Engine.h"

#include "Config.h"

#include "core/Debug.h"
#include "core/midi/MidiMessage.h"

#include "os/os.h"

Engine::Engine(Model &model, ClockTimer &clockTimer, Adc &adc, Dac &dac, Dio &dio, GateOutput &gateOutput, Midi &midi, UsbMidi &usbMidi) :
    _model(model),
    _dio(dio),
    _gateOutput(gateOutput),
    _midi(midi),
    _usbMidi(usbMidi),
    _cvInput(adc),
    _cvOutput(dac, model.settings().calibration()),
    _clock(clockTimer),
    _routingEngine(*this, model)
{
    _cvOutputOverrideValues.fill(0.f);
    _trackEngines.fill(nullptr);

    _usbMidi.setConnectHandler([this] (uint16_t vendorId, uint16_t productId) { usbMidiConnect(vendorId, productId); });
    _usbMidi.setDisconnectHandler([this] () { usbMidiDisconnect(); });
}

void Engine::init() {
    _cvInput.init();
    _cvOutput.init();
    _clock.init();

    initClock();
    updateClockSetup();

    // setup track engines
    updateTrackSetups();
    updateTrackSequences();
    resetTrackEngines();

    _lastSystemTicks = os::ticks();
}

void Engine::update() {
    uint32_t systemTicks = os::ticks();
    float dt = (0.001f * (systemTicks - _lastSystemTicks)) / os::time::ms(1);
    _lastSystemTicks = systemTicks;

    // locking
    if (_requestLock) {
        _clock.masterStop();
        _requestLock = 0;
        _locked = 1;
    }
    if (_requestUnlock) {
        _requestUnlock = 0;
        _locked = 0;
    }

    if (_locked) {
        // consume ticks
        uint32_t tick;
        while (_clock.checkTick(&tick)) {}

        // consume midi events
        MidiMessage message;
        while (_midi.recv(&message)) {}
        while (_usbMidi.recv(&message)) {}

        updateOverrides();
        _cvOutput.update();
        return;
    }

    // process clock events
    while (Clock::Event event = _clock.checkEvent()) {
        switch (event) {
        case Clock::Start:
            DBG("START");
            _running = true;
            resetTrackEngines();
            break;
        case Clock::Stop:
            DBG("STOP");
            _running = false;
            break;
        case Clock::Continue:
            DBG("CONTINUE");
            _running = true;
            break;
        case Clock::Reset:
            DBG("RESET");
            _running = false;
            resetTrackEngines();
            break;
        }
    }

    receiveMidi();

    // update tempo
    _nudgeTempo.update(dt);
    _clock.setMasterBpm(_model.project().bpm() + _nudgeTempo.strength() * 10.f);

    // update clock setup
    updateClockSetup();

    // update track setups
    updateTrackSetups();

    // update play state
    updatePlayState(false);

    // update cv inputs
    _cvInput.update();

    // update routings
    _routingEngine.update();

    uint32_t tick;
    bool updateOutputs = true;
    while (_clock.checkTick(&tick)) {
        _tick = tick;

        // update play state
        updatePlayState(true);

        for (auto trackEngine : _trackEngines) {
            trackEngine->tick(tick);
        }

        updateTrackOutputs();
        updateOutputs = false;
    }

    if (updateOutputs) {
        updateTrackOutputs();
    }

    for (auto trackEngine : _trackEngines) {
        trackEngine->update(dt);
    }

    updateOverrides();

    // update cv outputs
    _cvOutput.update();
}

void Engine::lock() {
    // TODO make re-entrant
    while (!isLocked()) {
        _requestLock = 1;
#ifdef PLATFORM_SIM
        update();
#endif
    }
}

void Engine::unlock() {
    while (isLocked()) {
        _requestUnlock = 1;
#ifdef PLATFORM_SIM
        update();
#endif
    }
}

bool Engine::isLocked() {
    return _locked == 1;
}

void Engine::clockStart() {
    _clock.masterStart();
}

void Engine::clockStop() {
    _clock.masterStop();
}

void Engine::clockContinue() {
    _clock.masterContinue();
}

void Engine::clockReset() {
    _clock.masterReset();
}

void Engine::tapTempoReset() {
    _tapTempo.reset(_model.project().bpm());
}

void Engine::tapTempoTap() {
    _tapTempo.tap();
    _model.project().setBpm(_tapTempo.bpm());
}

void Engine::nudgeTempoSetDirection(int direction) {
    _nudgeTempo.setDirection(direction);
}

float Engine::nudgeTempoStrength() const {
    return _nudgeTempo.strength();
}

float Engine::syncMeasureFraction() const {
    uint32_t measureDivisor = (_model.project().syncMeasure() * CONFIG_PPQN * 4);
    return float(_tick % measureDivisor) / measureDivisor;
}

bool Engine::sendMidi(MidiPort port, const MidiMessage &message) {
    switch (port) {
    case MidiPort::Midi:
        return _midi.send(message);
    case MidiPort::UsbMidi:
        return _usbMidi.send(message);
    }
    return false;
}

void Engine::showMessage(const char *text, uint32_t duration) {
    if (_messageHandler) {
        _messageHandler(text, duration);
    }
}

void Engine::setMessageHandler(MessageHandler handler) {
    _messageHandler = handler;
}

void Engine::onClockOutput(const Clock::OutputState &state) {
    _dio.clockOutput.set(state.clock);
    switch (_model.project().clockSetup().clockOutputMode()) {
    case ClockSetup::ClockOutputMode::Reset:
        _dio.resetOutput.set(state.reset);
        break;
    case ClockSetup::ClockOutputMode::Run:
        _dio.resetOutput.set(state.run);
        break;
    case ClockSetup::ClockOutputMode::Last:
        break;
    }
}

void Engine::onClockMidi(uint8_t data) {
    // TODO we should send a single byte with priority
    const auto &clockSetup = _model.project().clockSetup();
    if (clockSetup.midiTx()) {
        _midi.send(MidiMessage(data));
    }
    if (clockSetup.usbTx()) {
        _usbMidi.send(MidiMessage(data));
    }
}

void Engine::updateTrackSetups() {
    for (int trackIndex = 0; trackIndex < CONFIG_TRACK_COUNT; ++trackIndex) {
        const auto &track = _model.project().track(trackIndex);

        if (!_trackEngines[trackIndex] || _trackEngines[trackIndex]->trackMode() != track.trackMode()) {
            int linkTrack = track.linkTrack();
            const TrackEngine *linkedTrackEngine = linkTrack >= 0 ? &trackEngine(linkTrack) : nullptr;
            auto &trackEngine = _trackEngines[trackIndex];
            auto &trackContainer = _trackEngineContainers[trackIndex];

            switch (track.trackMode()) {
            case Track::TrackMode::Note:
                trackEngine = trackContainer.create<NoteTrackEngine>(track, linkedTrackEngine);
                break;
            case Track::TrackMode::Curve:
                trackEngine = trackContainer.create<CurveTrackEngine>(track, linkedTrackEngine);
                break;
            case Track::TrackMode::MidiCv:
                trackEngine = trackContainer.create<MidiCvTrackEngine>(track, linkedTrackEngine);
                break;
            case Track::TrackMode::Last:
                break;
            }

            const auto &trackState = _model.project().playState().trackState(trackIndex);
            trackEngine->setMute(trackState.mute());
            trackEngine->setFill(trackState.fill());
            trackEngine->setPattern(trackState.pattern());
        }

        _trackEngines[trackIndex]->setSwing(_model.project().swing());
    }
}

void Engine::updateTrackSequences() {
    auto &project = _model.project();

    for (int trackIndex = 0; trackIndex < CONFIG_TRACK_COUNT; ++trackIndex) {
        _trackEngines[trackIndex]->setPattern(project.playState().trackState(trackIndex).pattern());
    }
}

void Engine::updateTrackOutputs() {
    bool isIdle = _clock.isIdle();
    const auto &gateOutputTracks = _model.project().gateOutputTracks();
    const auto &cvOutputTracks = _model.project().cvOutputTracks();

    int trackGateIndex[CONFIG_TRACK_COUNT];
    int trackCvIndex[CONFIG_TRACK_COUNT];

    for (int trackIndex = 0; trackIndex < CONFIG_TRACK_COUNT; ++trackIndex) {
        trackGateIndex[trackIndex] = 0;
        trackCvIndex[trackIndex] = 0;
    }

    for (int trackIndex = 0; trackIndex < CONFIG_TRACK_COUNT; ++trackIndex) {
        if (trackIndex != _model.project().selectedTrackIndex()) {
            _trackEngines[trackIndex]->clearIdleOutput();
        }
        int gateOutputTrack = gateOutputTracks[trackIndex];
        if (!_gateOutputOverride) {
            if (isIdle && _trackEngines[gateOutputTrack]->idleOutput()) {
                _gateOutput.setGate(trackIndex, _trackEngines[gateOutputTrack]->idleGateOutput(trackGateIndex[gateOutputTrack]++));
            } else {
                _gateOutput.setGate(trackIndex, _trackEngines[gateOutputTrack]->gateOutput(trackGateIndex[gateOutputTrack]++));
            }
        }
        int cvOutputTrack = cvOutputTracks[trackIndex];
        if (!_cvOutputOverride) {
            if (isIdle && _trackEngines[cvOutputTrack]->idleOutput()) {
                _cvOutput.setChannel(trackIndex, _trackEngines[cvOutputTrack]->idleCvOutput(trackCvIndex[cvOutputTrack]++));
            } else {
                _cvOutput.setChannel(trackIndex, _trackEngines[cvOutputTrack]->cvOutput(trackCvIndex[cvOutputTrack]++));
            }
        }
    }
}

void Engine::resetTrackEngines() {
    for (auto trackEngine : _trackEngines) {
        trackEngine->reset();
    }
}

void Engine::updatePlayState(bool ticked) {
    auto &playState = _model.project().playState();
    auto &songState = playState.songState();
    const auto &song = _model.project().song();

    bool hasImmediateRequests = playState.hasImmediateRequests();
    bool hasSyncedRequests = playState.hasSyncedRequests();
    bool handleLatchedRequests = playState.executeLatchedRequests();
    bool hasRequests = hasImmediateRequests || hasSyncedRequests || handleLatchedRequests;

    uint32_t measureDivisor = (_model.project().syncMeasure() * CONFIG_PPQN * 4);
    bool handleSyncedRequests = (_tick % measureDivisor == 0 || _tick % measureDivisor == measureDivisor - 1);
    bool switchToNextSlot = ticked && (_tick % measureDivisor == measureDivisor - 1);

    // handle mute & pattern requests

    bool changedPatterns = false;

    if (hasRequests) {
        int muteRequests = PlayState::TrackState::ImmediateMuteRequest |
            (handleSyncedRequests ? PlayState::TrackState::SyncedMuteRequest : 0) |
            (handleLatchedRequests ? PlayState::TrackState::LatchedMuteRequest : 0);

        int patternRequests = PlayState::TrackState::ImmediatePatternRequest |
            (handleSyncedRequests ? PlayState::TrackState::SyncedPatternRequest : 0) |
            (handleLatchedRequests ? PlayState::TrackState::LatchedPatternRequest : 0);

        for (int trackIndex = 0; trackIndex < CONFIG_TRACK_COUNT; ++trackIndex) {
            auto &trackState = playState.trackState(trackIndex);

            // handle mute requests
            if (trackState.hasRequests(muteRequests)) {
                trackState.setMute(trackState.requestedMute());
            }

            // handle pattern requests
            if (trackState.hasRequests(patternRequests)) {
                trackState.setPattern(trackState.requestedPattern());
                changedPatterns = true;
            }

            // clear requests
            trackState.clearRequests(muteRequests | patternRequests);
        }
    }

    // handle song requests

    if (hasRequests) {
        int playRequests = PlayState::SongState::ImmediatePlayRequest |
            (handleSyncedRequests ? PlayState::SongState::SyncedPlayRequest : 0) |
            (handleLatchedRequests ? PlayState::SongState::LatchedPlayRequest : 0);

        int stopRequests = PlayState::SongState::ImmediateStopRequest |
            (handleSyncedRequests ? PlayState::SongState::SyncedStopRequest : 0) |
            (handleLatchedRequests ? PlayState::SongState::LatchedStopRequest : 0);

        if (songState.hasRequests(playRequests)) {
            int requestedSlot = songState.requestedSlot();
            if (requestedSlot >= 0 && requestedSlot < song.slotCount()) {
                const auto &slot = song.slot(requestedSlot);
                for (int trackIndex = 0; trackIndex < CONFIG_TRACK_COUNT; ++trackIndex) {
                    playState.trackState(trackIndex).setPattern(slot.pattern(trackIndex));
                }

                songState.setCurrentSlot(requestedSlot);
                songState.setCurrentRepeat(0);
                songState.setPlaying(true);
                switchToNextSlot = false;
            }
        }

        if (changedPatterns || songState.hasRequests(stopRequests)) {
            songState.setPlaying(false);
        }

        songState.clearRequests(playRequests | stopRequests);
    }

    // clear pending requests

    if (hasRequests) {
        playState.clearImmediateRequests();
        if (handleSyncedRequests) {
            playState.clearSyncedRequests();
        }
        if (handleLatchedRequests) {
            playState.clearLatchedRequests();
        }
    }

    // handle song slot change

    if (songState.playing() && switchToNextSlot) {
        const auto &slot = song.slot(songState.currentSlot());
        int currentSlot = songState.currentSlot();
        int currentRepeat = songState.currentRepeat();

        if (currentRepeat + 1 < slot.repeats()) {
            // next repeat
            songState.setCurrentRepeat(currentRepeat + 1);
        } else {
            // next slot
            songState.setCurrentRepeat(0);
            if (currentSlot + 1 < song.slotCount()) {
                songState.setCurrentSlot(currentSlot + 1);
            } else {
                songState.setCurrentSlot(0);
            }
        }

        // update patterns
        {
            const auto &slot = song.slot(songState.currentSlot());
            for (int trackIndex = 0; trackIndex < CONFIG_TRACK_COUNT; ++trackIndex) {
                playState.trackState(trackIndex).setPattern(slot.pattern(trackIndex));
                _trackEngines[trackIndex]->reset();
            }
        }
    }

    if (hasRequests | switchToNextSlot) {
        for (int trackIndex = 0; trackIndex < CONFIG_TRACK_COUNT; ++trackIndex) {
            auto &trackState = playState.trackState(trackIndex);
            auto &trackEngine = *_trackEngines[trackIndex];

            trackEngine.setMute(trackState.mute());
            trackEngine.setFill(trackState.fill());
            trackEngine.setPattern(trackState.pattern());
        }
    }
}

void Engine::updateOverrides() {
    // overrides
    if (_gateOutputOverride) {
        _gateOutput.setGates(_gateOutputOverrideValue);
    }
    if (_cvOutputOverride) {
        for (size_t i = 0; i < _cvOutputOverrideValues.size(); ++i) {
            _cvOutput.setChannel(i, _cvOutputOverrideValues[i]);
        }
    }
}

void Engine::usbMidiConnect(uint16_t vendorId, uint16_t productId) {
    if (_usbMidiConnectHandler) {
        _usbMidiConnectHandler(vendorId, productId);
    }
}

void Engine::usbMidiDisconnect() {
    if (_usbMidiDisconnectHandler) {
        _usbMidiDisconnectHandler();
    }
}

void Engine::receiveMidi() {
    MidiMessage message;
    while (_midi.recv(&message)) {
        receiveMidi(MidiPort::Midi, message);
    }
    while (_usbMidi.recv(&message)) {
        receiveMidi(MidiPort::UsbMidi, message);
    }
}

void Engine::receiveMidi(MidiPort port, const MidiMessage &message) {
    _midiLearn.receiveMidi(port, message);
    _routingEngine.receiveMidi(port, message);

    if (_midiReceiveHandler) {
        _midiReceiveHandler(port, message);
    }

    int channel = message.channel();
    for (auto trackEngine : _trackEngines) {
        trackEngine->receiveMidi(port, channel, message);
    }
}

void Engine::initClock() {
    _clock.setListener(this);

    const auto &clockSetup = _model.project().clockSetup();

    // Forward external clock signals to clock
    _dio.clockInput.setHandler([&] (bool value) {
        // interrupt context

        // start clock on first clock pulse if reset is not hold and clock is not running
        if (clockSetup.clockInputMode() == ClockSetup::ClockInputMode::Reset && !_clock.isRunning() && !_dio.resetInput.get()) {
            _clock.slaveStart(ClockSourceExternal);
        }
        if (value) {
            _clock.slaveTick(ClockSourceExternal);
        }
    });

    // Handle reset or start/stop input
    _dio.resetInput.setHandler([&] (bool value) {
        // interrupt context
        switch (clockSetup.clockInputMode()) {
        case ClockSetup::ClockInputMode::Reset:
            if (value) {
                _clock.slaveReset(ClockSourceExternal);
            } else {
                _clock.slaveStart(ClockSourceExternal);
            }
            break;
        case ClockSetup::ClockInputMode::Run:
            if (value) {
                _clock.slaveContinue(ClockSourceExternal);
            } else {
                _clock.slaveStop(ClockSourceExternal);
            }
            break;
        case ClockSetup::ClockInputMode::StartStop:
            if (value) {
                _clock.slaveStart(ClockSourceExternal);
            } else {
                _clock.slaveStop(ClockSourceExternal);
                _clock.slaveReset(ClockSourceExternal);
            }
            break;
        case ClockSetup::ClockInputMode::Last:
            break;
        }
    });

    // Forward MIDI clock messages to clock
    _midi.setRecvFilter([this] (uint8_t data) {
        if (MidiMessage::isClockMessage(data)) {
            _clock.slaveHandleMidi(ClockSourceMidi, data);
            return true;
        }
        return false;
    });

    _usbMidi.setRecvFilter([this] (uint8_t data) {
        if (MidiMessage::isClockMessage(data)) {
            _clock.slaveHandleMidi(ClockSourceUsbMidi, data);
            return true;
        }
        return false;
    });
}

void Engine::updateClockSetup() {
    auto &clockSetup = _model.project().clockSetup();

    if (!clockSetup.isDirty()) {
        return;
    }

    // Configure clock mode
    switch (clockSetup.mode()) {
    case ClockSetup::Mode::Auto:
        _clock.setMode(Clock::Mode::Auto);
        break;
    case ClockSetup::Mode::Master:
        _clock.setMode(Clock::Mode::Master);
        break;
    case ClockSetup::Mode::Slave:
        _clock.setMode(Clock::Mode::Slave);
        break;
    case ClockSetup::Mode::Last:
        break;
    }

    // Configure clock slaves
    _clock.slaveConfigure(ClockSourceExternal, clockSetup.clockInputDivisor(), true);
    _clock.slaveConfigure(ClockSourceMidi, CONFIG_PPQN / 24, clockSetup.midiRx());
    _clock.slaveConfigure(ClockSourceUsbMidi, CONFIG_PPQN / 24, clockSetup.usbRx());

    // Update from clock input signal
    bool resetInput = _dio.resetInput.get();
    bool running = _clock.isRunning();

    switch (clockSetup.clockInputMode()) {
    case ClockSetup::ClockInputMode::Reset:
        if (resetInput && running) {
            _clock.slaveReset(ClockSourceExternal);
        } else if (!resetInput && !running) {
            _clock.slaveStart(ClockSourceExternal);
        }
        break;
    case ClockSetup::ClockInputMode::Run:
        if (resetInput && !running) {
            _clock.slaveContinue(ClockSourceExternal);
        } else if (!resetInput && running) {
            _clock.slaveStop(ClockSourceExternal);
        }
        break;
    case ClockSetup::ClockInputMode::StartStop:
        if (resetInput && !running) {
            _clock.slaveStart(ClockSourceExternal);
        } else if (!resetInput && running) {
            _clock.slaveReset(ClockSourceExternal);
        }
        break;
    case ClockSetup::ClockInputMode::Last:
        break;
    }

    // Configure clock outputs
    _clock.outputConfigure(clockSetup.clockOutputDivisor(), clockSetup.clockOutputPulse());

    // Update clock outputs
    onClockOutput(_clock.outputState());

    clockSetup.clearDirty();
}
