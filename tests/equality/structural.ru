class (Point) [
x = 0
y = 0
constructor(a, b) [
self.x = a
self.y = b
]
]
p1 = new Point(1, 2)
p2 = new Point(1, 2)
p3 = new Point(3, 4)
if p1 == p2 [
printr("equal")
]
if p1 != p3 [
printr("different")
]
