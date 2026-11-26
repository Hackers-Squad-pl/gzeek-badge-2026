FROM python:3.12-slim-bookworm
ENV PYTHONDONTWRITEBYTECODE=1 PYTHONUNBUFFERED=1 PIP_NO_CACHE_DIR=1
RUN apt-get update \
    && apt-get install -y --no-install-recommends espeak-ng ffmpeg tzdata ca-certificates \
    && rm -rf /var/lib/apt/lists/* \
    && groupadd --gid 10001 gzeek \
    && useradd --uid 10001 --gid 10001 --no-create-home --shell /usr/sbin/nologin gzeek
WORKDIR /app
COPY requirements.txt ./
RUN pip install --no-cache-dir -r requirements.txt
COPY server.py smoke_test.py ./
USER 10001:10001
ENTRYPOINT ["python", "/app/server.py"]
