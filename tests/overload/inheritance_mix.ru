class (Base) [
fun handle(x) [
printr("base one", x)
]
]
class (Child : Base) [
fun handle() [
printr("child zero")
]
fun handle(x, y) [
printr("child two", x, y)
]
]
c = new Child()
c.handle()
c.handle(5)
c.handle(1, 2)
