"""User-facing errors shared by desktop and Android model imports."""


def message(error):
    if isinstance(error, ValueError):
        return str(error)
    if isinstance(error, OSError):
        return "Cannot access the selected files or save the model. Check file access and available storage."
    return "The model could not be imported. Your previous imported model has been preserved."
