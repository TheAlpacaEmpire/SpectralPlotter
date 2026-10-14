#pragma once

// Musically-grouped frequency ranges used to place the terrain peaks:
// 10 slices over the 64 log-spaced analysis bands (20 Hz–20 kHz), grouped so each slice
// covers a family of related instruments. Hoisted from TerrainModel::placePeaks.
namespace BandGroups
{
    inline constexpr int numGroups = 10;

    inline constexpr int starts[numGroups] = {  0,  6, 12, 18, 24, 29, 34, 39, 45, 53 };
    inline constexpr int ends  [numGroups] = {  5, 11, 17, 23, 28, 33, 38, 44, 52, 63 };

    inline constexpr int centerBand (int group) noexcept
    {
        return (starts[group] + ends[group]) / 2;
    }
}
