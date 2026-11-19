#/ String ordering (DECISIONS #129): UTF-8 byte comparison, prefix sorts
#/ first, ASCII uppercase sorts before lowercase (byte order), Persian text
#/ compares by code point.
printr("a" < "b")
printr("b" > "a")
printr("abc" < "abd")
printr("abc" < "abcd")
printr("abcd" >= "abcd")
printr("Zebra" < "apple")
printr("سارا" < "محمود")
printr("ali" != "reza")
