# Every source file that makes up the plugin. Shared by the plugin target and
# the headless test runner so both always compile identical code.
set(TB_SRC ${CMAKE_CURRENT_LIST_DIR}/../Source)

set(TB_PLUGIN_SOURCES
    # Host-facing shell
    ${TB_SRC}/PluginProcessor.cpp
    ${TB_SRC}/PluginEditor.cpp

    # Core: parameters, transport, lock-free bridges between threads
    ${TB_SRC}/Core/Parameters.cpp
    ${TB_SRC}/Core/TransportSync.cpp
    ${TB_SRC}/Core/SampleManager.cpp
    ${TB_SRC}/Core/PadLights.cpp
    ${TB_SRC}/Core/PushController.cpp
    ${TB_SRC}/Core/MikroUsb.cpp
    ${TB_SRC}/Core/MikroTester.cpp
    ${TB_SRC}/Core/MikroController.cpp
    ${TB_SRC}/Core/SnapshotManager.cpp
    ${TB_SRC}/Core/BreakBank.cpp
    ${TB_SRC}/Core/Mutator.cpp
    ${TB_SRC}/Core/Resampler.cpp

    # Engine: everything that runs inside processBlock()
    ${TB_SRC}/Engine/AudioEngine.cpp
    ${TB_SRC}/Engine/PadVoice.cpp
    ${TB_SRC}/Engine/SampleLoader.cpp
    ${TB_SRC}/Engine/TransientDetector.cpp
    ${TB_SRC}/Engine/MasterSection.cpp
    ${TB_SRC}/Engine/GroupBus.cpp
    ${TB_SRC}/Engine/FXChain.cpp

    # UI: the reactor control-room skin
    ${TB_SRC}/UI/ReactorLookAndFeel.cpp
    ${TB_SRC}/UI/PanelPainter.cpp
    ${TB_SRC}/UI/PadComponent.cpp
    ${TB_SRC}/UI/PadGrid.cpp
    ${TB_SRC}/UI/TransportDisplay.cpp
    ${TB_SRC}/UI/NeedleMeter.cpp
    ${TB_SRC}/UI/WaveformEditor.cpp
    ${TB_SRC}/UI/PadEditorPanel.cpp
    ${TB_SRC}/UI/Controls.cpp
    ${TB_SRC}/UI/FXGrid.cpp
    ${TB_SRC}/UI/MixerPanel.cpp
    ${TB_SRC}/UI/FxEditorPanel.cpp
    ${TB_SRC}/UI/SequencerPanel.cpp
    ${TB_SRC}/UI/SliceSeqPanel.cpp
    ${TB_SRC}/UI/SnapshotPanel.cpp
    ${TB_SRC}/UI/ResamplePanel.cpp
    ${TB_SRC}/UI/SceneStrip.cpp
    ${TB_SRC}/UI/MainView.cpp
)
