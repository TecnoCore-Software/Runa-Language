abstract class (Base) [
abstract fun go()
]
abstract class (Mid : Base) [
]
class (Leaf : Mid) [
fun go() [
printr("leaf goes")
]
]
l = new Leaf()
l.go()
