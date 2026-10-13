#include "PluginEditor.h"

using tb::ui::MainView;

TwistedBreaksEditor::TwistedBreaksEditor (TwistedBreaksProcessor& p)
    : AudioProcessorEditor (p), view (p)
{
    setLookAndFeel (&lookAndFeel);
    view.setLookAndFeel (&lookAndFeel);
    addAndMakeVisible (view);

    setResizable (true, true);
    setResizeLimits ((int) (MainView::kWidth * 0.75), (int) (MainView::kHeight * 0.75),
                     (int) (MainView::kWidth * 1.5),  (int) (MainView::kHeight * 1.5));
    if (auto* c = getConstrainer())
        c->setFixedAspectRatio ((double) MainView::kWidth / (double) MainView::kHeight);

    setSize (MainView::kWidth, MainView::kHeight);
}

TwistedBreaksEditor::~TwistedBreaksEditor()
{
    view.setLookAndFeel (nullptr);
    setLookAndFeel (nullptr);
}

void TwistedBreaksEditor::resized()
{
    const float scale = (float) getWidth() / (float) MainView::kWidth;
    view.setBounds (0, 0, MainView::kWidth, MainView::kHeight);
    view.setTransform (juce::AffineTransform::scale (scale));
}
