#!/bin/sh

PORT=47990

if ! curl -k https://localhost:$PORT > /dev/null 2>&1; then
  (sleep 3 && xdg-open https://localhost:$PORT) &
  exec vibepollo "$@"
else
  echo "Vibepollo is already running, opening the configuration API..."
  xdg-open https://localhost:$PORT
fi
