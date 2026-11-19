class (A) [
    fun go() [
        return 1
    ]
]
class (B : A) [
    override fun go() [
        return 2
    ]
]
printr(new B().go())
