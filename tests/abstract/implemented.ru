abstract class (Vehicle) [
abstract fun start()
]
class (Car : Vehicle) [
fun start() [
printr("Car started")
]
]
car = new Car()
car.start()
