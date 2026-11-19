def m500(self, **words):
    spindle = words.get("spindle", "omitted")
    if spindle != "omitted":
        spindle = int(spindle)
    print("M500 Python P={} spindle={}".format(int(words["p"]), spindle))


def m503(self, **words):
    spindle = int(words.get("spindle", 0))
    print("M503 Python spindle={} speed={}".format(spindle, int(words["s"])))
