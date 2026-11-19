class (Vehicle) [
fun start() [
printr("Vehicle")
]
]
class (Car : Vehicle) [
override fun start() [
super.start()
printr("Car")
]
]
car = new Car()
car.start()
