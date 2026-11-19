interface (Printable) [
    fun print()
]
class (Doc) implements Printable [
    fun print() [
        printr("printed")
    ]
]
d = new Doc()
d.print()
printr(d is Printable)
