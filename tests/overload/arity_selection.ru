class (Logger) [
fun log() [
printr("nothing")
]
fun log(value) [
printr("one:", value)
]
fun log(a, b) [
printr("two:", a, b)
]
]
logger = new Logger()
logger.log()
logger.log("Runa")
logger.log("Runa", 1)
