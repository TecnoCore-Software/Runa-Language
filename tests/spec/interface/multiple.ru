interface (A1) [
    fun one()
]
interface (B1) [
    fun two()
]
class (C) implements A1, B1 [
    fun one() []
    fun two() []
]
printr("multi ok")
