"""Run the server:  python -m server        (add --mock to simulate the hardware)"""
import os
import sys

if "--mock" in sys.argv:  # must be set before the config module is imported
    os.environ["MOCK_DEVICE"] = "1"

import uvicorn  # noqa: E402

from . import config  # noqa: E402

if __name__ == "__main__":
    uvicorn.run("server.app:app", host=config.HOST, port=config.PORT, log_level="info")
