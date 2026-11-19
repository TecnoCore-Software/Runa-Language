class (Vehicle) [
wheels = 4

fun start() [
printr("Vehicle with", self.wheels, "wheels started")
]
]

class (Car : Vehicle) [
brand = ""

constructor(brand) [
self.brand = brand
]

override fun start() [
super.start()
printr(self.brand, "is ready to drive")
]
]

class (Bike : Vehicle) [
constructor() [
self.wheels = 2
]

override fun start() [
super.start()
printr("pedaling away")
]
]

garage = new Car("Pride")
garage.start()

bike = new Bike()
bike.start()
