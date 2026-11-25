"""Results that are large, too large or not JSON."""


class Results:
    def __init__(self, document):
        pass

    def numbers(self, document):
        return list(range(document["count"]))

    def text(self, document):
        return "x" * document["count"]

    def nan(self, document):
        return {"value": float("nan")}

    def unserializable(self, document):
        return {"value": {1, 2}}
