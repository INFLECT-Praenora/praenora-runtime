"""Echo: gives back what it is given."""


class Echo:
    def __init__(self, document):
        self.document = document

    def echo(self, document):
        return document

    def created_with(self, document):
        return self.document

    def nothing(self, document):
        return None

    not_a_method = 1
