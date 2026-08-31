# LCL Settings uygulaması ve adaptif kullanıcı-arayüzü

Bu plan native C++ `Settings.app` ile onu mümkün kılan genel LCL UI
primitive'lerini tanımlar. Settings, sandbox/permission backend'ini oluşturmaz;
yalnız onun dar, root-owned admin capability'sini tüketir. Aynı canonical LCL
kullanıcı alanı binary'si Linux ve Android substrate'larında çalışır.

## Değişmez kurallar

- [ ] Settings root olarak çalışmaz; normal uygulamalara verilmeyen, yalnız
  security yönetimine ait dar capability ile çalışır.
- [ ] İlk Settings sürümünü native C++ olarak uygula; `lcl-js` için security
  admin capability'si veya bu capability FD'sini açan bir binding ekleme.
- [ ] Settings'in UI'sı sandbox, imza doğrulama veya permission kararını
  değiştiremez; backend'in exact kimlik ve policy sonucunu gösterir.

## 1. Settings sistem kimliği ve admin capability zinciri

- [ ] `/System/Applications/Settings.app` altında immutable, canonical
  `org.lcl.settings` system bundle'ını paketle; aynı app ID'nin user/machine
  bundle tarafından override edilmesini reddet.
- [ ] sessiond, yalnız doğrulanmış canonical Settings bundle'ı başlatılırken
  root-owned `lcl-securityd` bağlantısını açıp bu tek amaçlı, önceden bağlı
  capability FD'sini Settings sürecine aktaracak zinciri kur.
- [ ] `lcl-securityd` dinleme socket'ini session-user veya normal app UID'sine
  değil, yalnız root/sessiond'ye aç; normal sandbox runtime görünümüne socket
  yolu bind etme.
- [ ] Aktarılan capability'nin yalnız pending bundle listesi, exact
  `app ID + BundleRecord hash` ile Allow ve aynı exact kimlikle Remove Allow
  işlemlerini yapabildiğini doğrula; generic store, launch, elevation veya
  dosya sistemi admin işlemi vermeme.
- [ ] Settings'e root UID, `lcl-sandboxd` kontrol socket'i, keyfî FD, mount,
  exec veya başka app'in private `/Data` görünümünü verme.
- [ ] sessiond'nin capability FD'sini yalnız system Settings'e verdiğini;
  bundle path değişimi, sahte `org.lcl.settings`, direct socket connect ve
  FD inheritance kaçışına karşı test et.

## 2. Adaptif kullanıcı-arayüzü ortamı

- [ ] `WindowApp` üzerinden backend-bağımsız `LayoutEnvironment` sağla:
  logical kullanılabilir genişlik/yükseklik, safe-area inset'leri ve türetilmiş
  `Compact`/`Expanded` size class. Android veya Linux'a göre ayrı UI dalı
  oluşturma.
- [ ] Safe-area değerlerini Gestalt/platform katmanından UI'a typed API ile
  aktar; uygulama kodu çentik, status bar veya gesture alanı için sabit piksel
  varsayımı yapmasın.
- [ ] `Widget` için, Yoga'yı public API yapmadan layout'tan tamamen çıkan bir
  `LayoutVisibility::Collapsed` sözleşmesi ekle. Mevcut `setVisible(false)`
  yalnız çizimi kapattığı için sidebar collapse yerine kullanma.
- [ ] Size class değişiminde mevcut route, seçili ayar, scroll offset, focus ve
  taslak kullanıcı girdisini koruyan tek bir Settings state modeli tanımla.
- [ ] Breakpoint politikasını platform/model adına göre değil minimum sidebar
  ve detail ölçülerine göre belirle; sınırın iki yanında deterministik test
  et.

## 3. LCL UI navigasyon ve liste primitive'leri

- [ ] `NavigationStack` ekle: typed route kimliği, push/pop/replace ve
  transition bitene kadar outgoing/incoming page ömür yönetimi.
- [ ] Compact navigation pop işlemini yalnız görünür başlık geri kontrolüyle
  başlat; LCL'de hardware-back UI olayı veya platform back tuşu/gesture'ını
  route pop'a bağlayan public API oluşturma.
- [ ] `PageTransition` sözleşmesini `NavigationStack` içinde tanımla:
  başlangıçta `None`, `Push`, `Pop` ve `Replace`; yön, interrupt ve completion
  davranışları belirlenmiş olsun.
- [ ] `Push` geçişinde container genişliği `W` iken eski sayfayı `0 → -0.5W`,
  yeni sayfayı `+1.0W → 0` yatay translation ile birlikte animate et.
- [ ] `Pop` geçişinde mevcut sayfayı `0 → +1.0W`, açığa çıkan önceki sayfayı
  `-0.5W → 0` yatay translation ile animate et; iki sayfayı transition
  tamamlanana kadar canlı ve clip'lenmiş tut.
- [ ] `Replace` için ayrı, yönsüz bir transition tanımla; sidebar sabitken
  detail route değişiminde Push/Pop stack hareketini kullanma.
- [ ] `NavigationSplitView` ekle: Expanded modda sidebar ve detail eşzamanlı
  görünür; Compact modda aynı route state ile sidebar/list ve detail arasında
  stack üzerinden geçer. Layout değişiminde page state'i yeniden yaratma.
- [ ] `ListView` ekle: veri kaynağı, stable item key, görünür satır üretimi,
  reuse/virtualization, selection, klavye focus ve scroll-position geri
  yükleme sözleşmelerini tanımla.
- [ ] Generic `NavigationList` ve selection-aware row görünümünü ekle; hover,
  touch, klavye ve erişilebilir focus davranışları platformlar arasında aynı
  olsun.
- [ ] `AlertDialog` ekle: başlık, mesaj, varsayılan/cancel/destructive action,
  scrim, focus scope, pointer dışı tıklama politikası ve accept/cancel
  completion semantiği tanımlansın.
- [ ] `ActionSheet` ekle: kısa context eylem listesi, destructive action ve
  görünür cancel kontrolü sağla.
- [ ] `Sheet` ekle: daha uzun form veya seçim akışları için sunum/dismiss
  yaşam döngüsünü tanımla; `AlertDialog` yerine kullanma.
- [ ] Arama alanını mevcut `TextField` üstünde Settings-özel bir composition
  olarak tasarla; ilk Security MVP için global arama index'i veya platform
  özel arama API'si gerektirme.
- [ ] Settings-özel `SettingsSection` ve `SettingsRow` bileşenlerini mevcut
  `Container`, `Text`, `Image`, `Divider` ve `Toggle` ile compose et; bunları
  genel LCL UI primitive'i yapma.

## 4. Settings uygulaması MVP: Güvenlik ekranı

- [ ] Native C++ `Settings.app` hedefini, manifestini, CMake entegrasyonunu ve
  canonical rootfs packaging'ini ekle; Linux ve Android aynı binary/user-space
  uygulamasını çalıştırsın.
- [ ] İlk route'u `Security` yap: bekleyen doğrulanamayan bundle'ların adı,
  app ID'si, bundle yolu, publisher durumu ve kısa BundleRecord hash bilgisini
  `lcl-securityd` capability üzerinden göster.
- [ ] Bir bundle satırı seçildiğinde tam hash, istenen izinler ve kullanıcıya
  etkisini gösteren detail sayfasını aç; Allow için `AlertDialog` kullan.
- [ ] Allow/Remove Allow işlemini yalnız seçili exact `app ID + hash` ile
  gönder; backend kaydın değiştiğini bildirirse ekranı yenile ve eski seçimi
  kabul etme.
- [ ] Allow sonrasında bundle'ın normal sandbox altında yeniden açılabileceğini;
  bunun imza, root veya elevation izni vermediğini kullanıcı arayüzünde açıkça
  belirt.

## 5. Sonraki Settings sayfaları

- [ ] Uygulama başına etkin izinleri, izin kaynağını ve son karar zamanını
  listeleyen ekranı tasarla; grant/revoke kontrollerini PermissionStore admin
  API'sine bağla.
- [ ] Yeniden başlatma gerektiren izin değişikliklerini açıkça göster.
- [ ] Uygulama elevation geçmişini, aktif elevated oturumları ve revoke
  kontrolünü göster.
- [ ] Uygulama verisini silme, izinleri sıfırlama ve uygulamayı kaldırma
  eylemlerini birbirinden ayır.

## 6. Settings ve adaptif UI doğrulaması

- [ ] Host testleri: collapsed widget layout alanı kaplamaz; safe area ve size
  class değişimi doğru bildirilir; sidebar/detail geçişinde state korunur.
- [ ] Host testleri: navigation push/pop/replace transition'ları eski page'i
  doğru zamanda bırakır; Back/Escape ve hızlı ardışık route değişimleri stale
  callback veya focus sızıntısı oluşturmaz.
- [ ] Host testleri: ListView stable key, recycle, selection ve scroll restore
  davranışını doğrula.
- [ ] Security integration testleri: yalnız canonical Settings capability FD
  ile list/Allow/Remove Allow yapılır; normal sandbox app'i, sahte Settings
  bundle'ı ve direct `lcl-securityd` client'ı reddedilir.
- [ ] QEMU ve Android kullanıcı kabulü: Compact ekranda liste → detail → Back,
  Expanded ekranda sidebar + detail, safe-area ve resize/orientation değişimi
  doğrulanır; pending unsigned bundle Allow sonrası yalnız normal sandbox ile
  başlar.
