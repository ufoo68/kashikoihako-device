# KASHIKOIHAKO

For device

## build

```
activate edukit
%idf_path%/export.bat
```

## register certificate

```
cd utilities/AWS_IoT_registration_helper
python registration_helper.py -p <<DEVICE_PORT>>
```