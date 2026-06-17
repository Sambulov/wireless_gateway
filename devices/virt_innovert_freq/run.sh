PORT=${1:-"/dev/ttyUSB1"}
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

echo "communication port: ${PORT}. To change it pass it in param"

pkill -f innovert_virtual.py
python3 "${SCRIPT_DIR}/innovert_virtual.py" --port ${PORT} --baud 115200
