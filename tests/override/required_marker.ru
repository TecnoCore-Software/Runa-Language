class (Vehicle) [
fun start() [
printr("vehicle")
]
]
class (Car : Vehicle) [
override fun start() [
printr("car")
]
]
new Car().start()
