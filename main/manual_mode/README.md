# Manual Mode

## Endpoint on ESP32

- `POST /manual-analyze`
- Returns:

```json
{
  "ok": true,
  "id": 3
}
```

## Trigger Example

```bash
curl -X POST http://<esp32-ip>/manual-analyze
```

## Data Flow

1. Flutter calls `POST /manual-analyze` on ESP32.
2. ESP32 captures current USB camera frame.
3. ESP32 uploads JPEG frame to backend `POST /analyze-egg`.
4. ESP32 returns parsed `id` to Flutter.
