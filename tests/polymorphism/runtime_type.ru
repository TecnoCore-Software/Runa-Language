class (Vehicle) [
fun start() [
printr("Vehicle")
]
]
class (Car : Vehicle) [
override fun start() [
printr("Car")
]
]
class (Boat : Vehicle) [
override fun start() [
printr("Boat")
]
]
garage = new Car()
harbor = new Boat()
garage.start()
harbor.start()
