class (Vehicle) [
]
class (Car : Vehicle) [
]
class (SportCar : Car) [
]
s = new SportCar()
if s is SportCar [
printr("SportCar")
]
if s is Car [
printr("Car")
]
if s is Vehicle [
printr("Vehicle")
]
