class (Car) [
static built = 0
constructor() [
Car.built = Car.built + 1
]
]
c1 = new Car()
c2 = new Car()
c3 = new Car()
printr(Car.built)
