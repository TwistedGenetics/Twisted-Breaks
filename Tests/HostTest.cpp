/*
    TwistedBreaksHostTest — loads the BUILT plugin binary (VST3) through a
    real plugin host (JUCE's VST3 hosting), the same route Ableton uses, and
    checks the things that only break across that boundary:
      * host save -> new instance -> host restore (Live Set save / duplicate)
      * parameter values seen by the host after a restore
      * host-side parameter set (undo / automation path)
    Usage: TwistedBreaksHostTest /path/to/TwistedBreaks.vst3
*/
#include <JuceHeader.h>
#include <iostream>

static int failures = 0, checks = 0;
static void check (bool ok, const juce::String& what)
{
    ++checks; if (! ok) ++failures;
    std::cout << (ok ? "  [PASS] " : "  [FAIL] ") << what << std::endl;
}

static juce::AudioProcessorParameter* findParam (juce::AudioPluginInstance& p, const juce::String& name)
{
    for (auto* prm : p.getParameters())
        if (prm->getName (100) == name) return prm;
    return nullptr;
}

int main (int argc, char* argv[])
{
    juce::ScopedJuceInitialiser_GUI gui;
    if (argc < 2) { std::cout << "usage: TwistedBreaksHostTest plugin.vst3\n"; return 2; }

    juce::AudioPluginFormatManager fm;
    juce::String path (argv[1]);
   #if JUCE_PLUGINHOST_AU && JUCE_MAC
    const bool isAU = path.endsWith (".component") || path.startsWith ("AudioUnit:");
    if (isAU) fm.addFormat (new juce::AudioUnitPluginFormat());
    else
   #endif
    fm.addFormat (new juce::VST3PluginFormat());

    juce::OwnedArray<juce::PluginDescription> types;
    auto* format = fm.getFormat (0);
    if (path.endsWith (".component"))
    {
        // AU: find the installed component by name via a scan of the system list.
        for (auto& id : format->searchPathsForPlugins ({}, true, false))
            if (id.containsIgnoreCase ("TwistedBreaks") || id.containsIgnoreCase ("Twbk"))
                format->findAllTypesForFile (types, id);
    }
    else
    {
        format->findAllTypesForFile (types, path);
    }
    if (types.isEmpty()) { std::cout << "could not load " << argv[1] << "\n"; return 2; }

    auto make = [&]
    {
        juce::String err;
        auto inst = fm.createPluginInstance (*types[0], 48000.0, 512, err);
        if (inst == nullptr) std::cout << "instance error: " << err << "\n";
        else inst->prepareToPlay (48000.0, 512);
        return inst;
    };

    auto pump = [] (int ms) { juce::MessageManager::getInstance()->runDispatchLoopUntil (ms); };
    auto block = [] (juce::AudioPluginInstance& p)
    {
        juce::AudioBuffer<float> b (2, 512); juce::MidiBuffer m;
        p.processBlock (b, m);
    };

    std::cout << "== Host round trip (" << types[0]->name << ", " << types[0]->pluginFormatName << ") ==\n";
    auto a = make();
    if (a == nullptr) return 2;

    auto* gainA = findParam (*a, "Master Level");
    auto* limA  = findParam (*a, "Master Limiter");
    auto* pitchA = findParam (*a, "Pad 03 Pitch");
    check (gainA && limA && pitchA, "host sees Master Level / Master Limiter / Pad 03 Pitch");
    if (! (gainA && limA && pitchA)) return 1;

    std::cout << "  params visible to host: " << a->getParameters().size() << "\n";
    std::cout << "  default Master Level text: " << gainA->getCurrentValueAsText() << "\n";

    // -7.5 dB via the host (like a knob move that Live records)
    const float target = gainA->getValueForText ("-7.5");
    gainA->setValueNotifyingHost (target);
    limA->setValueNotifyingHost (0.0f);
    pitchA->setValueNotifyingHost (0.25f);
    block (*a); pump (50);
    std::cout << "  A after set: gain=" << gainA->getCurrentValueAsText() << " limiter=" << limA->getCurrentValueAsText() << "\n";
    check (std::abs (gainA->getValue() - target) < 1e-4f, "host-set value sticks in instance A");

    juce::MemoryBlock blob;
    a->getStateInformation (blob);
    std::cout << "  state blob: " << (int) blob.getSize() << " bytes\n";

    auto b = make();
    b->setStateInformation (blob.getData(), (int) blob.getSize());
    block (*b); pump (100);
    auto* gainB = findParam (*b, "Master Level");
    auto* limB  = findParam (*b, "Master Limiter");
    auto* pitchB = findParam (*b, "Pad 03 Pitch");
    std::cout << "  B after restore: gain=" << gainB->getCurrentValueAsText() << " limiter=" << limB->getCurrentValueAsText()
              << " pad3 pitch=" << pitchB->getCurrentValueAsText() << "\n";
    check (std::abs (gainB->getValue() - target) < 1e-3f, "new instance restores Master Level (save/reopen, duplicate track)");
    check (limB->getValue() < 0.5f, "new instance restores limiter OFF");
    check (std::abs (pitchB->getValue() - 0.25f) < 1e-3f, "new instance restores per-pad pitch");

    // Undo path: host sets the old value back.
    gainB->setValueNotifyingHost (gainB->getValueForText ("0"));
    block (*b); pump (50);
    check (gainB->getCurrentValueAsText().startsWith ("0.0"), "host can set the value back (undo): " + gainB->getCurrentValueAsText());

    // Editor open during restore (Ableton often has the window open).
    std::unique_ptr<juce::AudioProcessorEditor> ed (b->createEditorIfNeeded());
    pump (100);
    b->setStateInformation (blob.getData(), (int) blob.getSize());
    block (*b); pump (150);
    check (std::abs (gainB->getValue() - target) < 1e-3f, "restore with the editor open");
    ed.reset();

    a.reset(); b.reset();
    std::cout << "\n" << (checks - failures) << " / " << checks << " host checks passed\n";
    return failures == 0 ? 0 : 1;
}
