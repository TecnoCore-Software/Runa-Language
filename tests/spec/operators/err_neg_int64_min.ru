#/ -(Int64 min) has no positive counterpart: clean RuntimeError, never
#/ silent wrap-around (DECISIONS #126 overflow policy).
x = -9223372036854775807 - 1
printr(-x)
