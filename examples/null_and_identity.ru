
class (Car) [
name = "mystery"
]

car = null
if car == null [
printr("No car")
]

car = new Car()
if car == null [
printr("impossible")
] else [
printr("car exists:", car.name)
]

alias = car
if car === alias [
printr("same object through alias")
]

other = new Car()
if car === other [
printr("same")
] else [
printr("two distinct cars")
]

if car is Car [
printr("car is a Car")
]
car = null
if car is Car [
printr("impossible")
] else [
printr("null is not a Car")
]
