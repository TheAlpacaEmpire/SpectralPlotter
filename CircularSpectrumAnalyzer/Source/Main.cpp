/*
    Spectral Plotter — an audio-reactive topographic plotter on a simulated phosphor CRT.
    Copyright (C) 2026. Licensed under the GNU General Public License v3 (see LICENSE).
*/

#include <JuceHeader.h>
#include "MainComponent.h"

//==============================================================================
class CircularSpectrumAnalyzerApplication  : public juce::JUCEApplication
{
public:
    //==============================================================================
    CircularSpectrumAnalyzerApplication() {}

    const juce::String getApplicationName() override       { return "Spectral Plotter"; }
    const juce::String getApplicationVersion() override    { return ProjectInfo::versionString; }
    bool moreThanOneInstanceAllowed() override             { return true; }

    //==============================================================================
    void initialise (const juce::String& commandLine) override
    {
        juce::ignoreUnused (commandLine);
        mainWindow.reset (new MainWindow (getApplicationName()));
    }

    void shutdown() override
    {
        if (mainWindow != nullptr)
            if (auto* mc = dynamic_cast<MainComponent*> (mainWindow->getContentComponent()))
                mc->saveSettings();

        mainWindow = nullptr;
    }

    //==============================================================================
    void systemRequestedQuit() override
    {
        quit();
    }

    void anotherInstanceStarted (const juce::String& commandLine) override
    {
        juce::ignoreUnused (commandLine);
    }

    //==============================================================================
    /*
        This class implements the desktop window that contains an instance of
        our MainComponent class.
    */
    class MainWindow    : public juce::DocumentWindow
    {
    public:
        MainWindow (juce::String name)
            : DocumentWindow (name,
                              juce::Desktop::getInstance().getDefaultLookAndFeel()
                                                          .findColour (juce::ResizableWindow::backgroundColourId),
                              DocumentWindow::allButtons)
        {
            // Borderless: no OS/JUCE title bar — the app draws its own CRT header, and the
            // window is moved by dragging that header (see MainComponent::mouseDown/Drag).
            setUsingNativeTitleBar (false);
            setTitleBarHeight (0);
            setContentOwned (new MainComponent(), true);

           #if JUCE_IOS || JUCE_ANDROID
            setFullScreen (true);
           #else
            setResizable (true, true);
            // Keep the terminal usable: map box stays >= ~440 px at the minimum size.
            setResizeLimits (540, 580, 10000, 10000);
            centreWithSize (getWidth(), getHeight());
           #endif

            setVisible (true);

            if (auto* mc = dynamic_cast<MainComponent*> (getContentComponent()))
                mc->restoreWindowState();   // saved size/position/fullscreen, if still on-screen
        }

        void closeButtonPressed() override
        {
            JUCEApplication::getInstance()->systemRequestedQuit();
        }

    private:
        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainWindow)
    };

private:
    std::unique_ptr<MainWindow> mainWindow;
};

//==============================================================================
// This macro generates the main() routine that launches the app.
START_JUCE_APPLICATION (CircularSpectrumAnalyzerApplication)
