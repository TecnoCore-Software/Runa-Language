#/ Logical operators: and / or / not — real short-circuit evaluation.
age = 35
printr(true and false)
printr(true or false)
printr(not false)
if age > 18 and age < 60 [
    printr("middle of life")
]
#/ the right side is never evaluated when the left side decides:
printr(false and (1 / 0 == 1))
printr(true or (1 / 0 == 1))
