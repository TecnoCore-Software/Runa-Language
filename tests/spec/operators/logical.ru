#/ Logical operators (DECISIONS #127): and/or/not on booleans only, Python
#/ precedence (or < and < not), short-circuit evaluation — the right side of
#/ and/or is NOT evaluated when the left side decides.
printr(true and false)
printr(true and true)
printr(false or true)
printr(false or false)
printr(not false)
printr(not not true)
printr(not true == false)
#/ short-circuit: 1 / 0 would crash if evaluated — it never runs.
printr(false and (1 / 0 == 1))
printr(true or (1 / 0 == 1))
age = 35
if age > 18 and age < 60 [
    printr("middle")
]
if age < 12 or age > 65 [
    printr("edge")
] else [
    printr("in range")
]
