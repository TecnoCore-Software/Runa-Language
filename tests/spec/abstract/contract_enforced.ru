abstract class (Shape) [
    abstract fun area()
]
class (Square : Shape) [
    side = 0
    constructor(s) [
        self.side = s
    ]
    override fun area() [
        return self.side * self.side
    ]
]
s = new Square(4)
printr(s.area())
