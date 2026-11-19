abstract class (Game) [
abstract fun greet()

fun run() [
self.greet()
printr("running")
]
]
class (Chess : Game) [
fun greet() [
printr("chess!")
]
]
g = new Chess()
g.run()
