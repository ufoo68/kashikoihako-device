# KASHIKOIHAKO

For device

## build

```bash
pio run --environment core2foraws
```

## upload

```bash
pio run --environment core2foraws --target upload
```

## register certificate

```bash
cd utilities/AWS_IoT_registration_helper
python registration_helper.py -p <<DEVICE_PORT>>
```
