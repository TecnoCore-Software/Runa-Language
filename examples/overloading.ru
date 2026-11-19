
class (Logger) [
fun log() [
printr("[log] (nothing)")
]

fun log(value) [
printr("[log]", value)
]

fun log(a, b) [
printr("[log]", a, b)
]

fun repeat(times) [
if times > 0 [
self.log("tick", times)
self.repeat(times - 1)
]
]
]

logger = new Logger()
logger.log()
logger.log("Runa")
logger.log("Runa", 1)
logger.repeat(2)
