#pragma once

#include <JuceHeader.h>

//==============================================================================
// Persistent user settings (knob values, palette, audio source, window placement),
// stored as XML at %APPDATA%\SpectralPlotter\SpectralPlotter.settings. Message thread only.
// Loaded once at startup and written on shutdown; a missing or unreadable file just
// means defaults.
class AppSettings
{
public:
    AppSettings()
    {
        juce::PropertiesFile::Options o;
        o.applicationName     = "SpectralPlotter";
        o.folderName          = "SpectralPlotter";
        o.filenameSuffix      = ".settings";
        o.osxLibrarySubFolder = "Application Support";
        o.storageFormat       = juce::PropertiesFile::storeAsXML;
        o.millisecondsBeforeSaving = -1;   // only on explicit save()
        file = std::make_unique<juce::PropertiesFile> (o);
    }

    juce::PropertiesFile& props() noexcept { return *file; }

    // Clamped float read, so a hand-edited or stale file can't push a knob out of range.
    float getFloat (const juce::String& key, float def, float lo, float hi) const
    {
        return juce::jlimit (lo, hi, (float) file->getDoubleValue (key, def));
    }

    bool save() { return file->save(); }

private:
    std::unique_ptr<juce::PropertiesFile> file;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AppSettings)
};
