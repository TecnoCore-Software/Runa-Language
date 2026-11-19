interface (Printable) [
fun print()
]
abstract class (Base) implements Printable [
name = "base"
]
class (Child : Base) [
fun print() [
printr(self.name)
]
]
c = new Child()
c.print()
if c is Printable [
printr("still printable")
]
