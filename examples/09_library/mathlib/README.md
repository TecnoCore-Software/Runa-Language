// mathlib — a Runa Library package (examples/09_library).
//
// Pack it and use it from any program:
//
//     rpr pack mathlib math.rpr
//     mkdir -p runalibs && mv math.rpr runalibs/
//
//     # main.ru
//     import math
//     printr(math.sqrt(16))
//
// Package layout: manifest.json + init.ru (the public entry point).
// Top-level functions and classes are exported; top-level variables stay
// internal to the package (hidden state).
{
