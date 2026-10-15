# 智能链接部署说明

## 文件说明

- `bind.html` — 设备绑定中间页

## 部署步骤

### 1. 上传到服务器

将 `bind.html` 上传到 `jiuxiaoyw.online` 服务器的 Web 根目录。

### 2. 配置路由

确保以下 URL 可访问：
- `https://jiuxiaoyw.online/bind?sn=xxx&pin=xxx`

Nginx 配置示例：
```nginx
server {
    listen 443 ssl;
    server_name jiuxiaoyw.online;
    
    # SSL 证书配置
    ssl_certificate /path/to/cert.pem;
    ssl_certificate_key /path/to/key.pem;
    
    # 绑定页面
    location /bind {
        alias /var/www/bind.html;
        try_files $uri =404;
    }
    
    # 下载页面（可选）
    location /download {
        return 302 https://jiuxiaoyw.online/app;
    }
}
```

### 3. App 端配置

#### Android (AndroidManifest.xml)
```xml
<activity android:name=".BindActivity"
          android:launchMode="singleTask">
    <intent-filter>
        <action android:name="android.intent.action.VIEW" />
        <category android:name="android.intent.category.DEFAULT" />
        <category android:name="android.intent.category.BROWSABLE" />
        <data android:scheme="csinv"
              android:host="bind" />
    </intent-filter>
</activity>
```

#### iOS (Info.plist)
```xml
<key>CFBundleURLTypes</key>
<array>
    <dict>
        <key>CFBundleURLSchemes</key>
        <array>
            <string>csinv</string>
        </array>
    </dict>
</array>
```

### 4. 测试

1. 生成铭牌二维码
2. 用手机相机扫描
3. 已安装 App → 自动打开 App 并填入 SN+PIN
4. 未安装 App → 显示下载引导页

## 自定义 Scheme

- Scheme: `csinv`
- Host: `bind`
- 参数: `sn`, `pin`
- 完整 URL: `csinv://bind?sn=xxx&pin=xxx`

## 注意事项

1. iOS 9+ 需要 HTTPS
2. Android 6.0+ 需要 App Links 验证
3. 微信/QQ 内置浏览器可能拦截 Scheme 跳转
4. 建议在页面中添加"在浏览器中打开"提示
