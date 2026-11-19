class (Car) [
    public speed = 0
    constructor(base) [
        self.speed = base
    ]
    fun accelerate(times) [
        loop times > 0 [
            self.speed = self.speed + 10
            times = times - 1
        ]
    ]
    fun getSpeed() [
        return self.speed
    ]
]
car = new Car(5)
car.accelerate(4)
printr(car.getSpeed())
loop car.getSpeed() < 60 [
    car.speed = car.speed + 15
]
printr(car.getSpeed())
