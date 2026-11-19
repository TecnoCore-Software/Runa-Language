class (M) [
fun sign(x) [
if x > 0 [
return "positive"
] elif x < 0 [
return "negative"
] else [
return "zero"
]
]
]
m = new M()
printr(m.sign(5))
printr(m.sign(0 - 3))
printr(m.sign(0))
