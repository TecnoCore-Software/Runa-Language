
class (Car) [
static built = 0
static factory = "Runa Motors"

constructor() [
Car.built = Car.built + 1
]

static fun fleetSize() [
return Car.built
]
]

new Car()
new Car()
new Car()

printr(Car.factory)
printr("cars built:", Car.fleetSize())
printr("cars built:", Car.built)
