class (A) [
fun value() [
return 10
]
]
class (B : A) [
override fun value() [
return super.value() + 5
]
]
b = new B()
printr(b.value())
