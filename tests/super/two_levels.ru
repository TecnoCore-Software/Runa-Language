class (A) [
fun go() [
printr("A")
]
]
class (B : A) [
override fun go() [
super.go()
printr("B")
]
]
class (C : B) [
override fun go() [
super.go()
printr("C")
]
]
c = new C()
c.go()
