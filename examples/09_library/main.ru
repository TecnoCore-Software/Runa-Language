
import math
as "math" import {sqrt, Vector}

printr(math.sqrt(16))
printr(math.add(2, 3))

printr(sqrt(25))

v = math.Vector.make(3, 4)
printr(v.length2())
printr(math.Vector.count)

w = new Vector(1, 1)
if w is Vector [
    printr("w is a Vector")
]
if w is math.Vector [
    printr("w is math.Vector")
]
