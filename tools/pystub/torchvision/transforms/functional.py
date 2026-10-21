# placeholder for spandrel's imports; the upscaling models never call these
import enum


class InterpolationMode(enum.Enum):
    NEAREST = "nearest"
    BILINEAR = "bilinear"
    BICUBIC = "bicubic"


def rotate(*a, **k):
    raise NotImplementedError("torchvision is not installed")


def normalize(*a, **k):
    raise NotImplementedError("torchvision is not installed")
