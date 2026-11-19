class (A) [
    fun hi() [
        printr("from A")
    ]
]
class (B : A) [
    override fun hi() [
        super.hi()
        printr("from B")
    ]
]
b = new B()
b.hi()
