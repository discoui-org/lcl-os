# LCL uygulama sandbox ve izin sistemi

Bu plan, LCL uygulamalarını Docker/OCI kullanmadan Linux kernel sınırlarıyla
izole eder. Hedef; canonical LCL kullanıcı alanında çalışan aynı uygulama
binary'lerinin bare-metal Linux ve Android substrate'larında aynı güvenlik
sözleşmesini kullanmasıdır.

## Başlangıç noktası

- [ ] Mevcut davranışı referans olarak kaydet: `lcl-sessiond`, uygulamaları
  doğrudan `fork()` ve `execv()` ile başlatıyor; uygulama sandbox'ı yok.
- [ ] Mevcut canonical rootfs başlangıcında root, sessiond, shell ve uygulama
  süreçlerinin gerçek UID/GID/capability değerlerini kaydet.
- [ ] Mevcut Android bootstrap zincirinde `lcl-bootstrap`, `lcl-core-android`,
  rootfs session ve uygulamaların SELinux context'lerini kaydet.
- [ ] Mevcut QEMU kernel ve hedef Android kernel için namespace, seccomp,
  cgroup v2 ve Landlock özelliklerini runtime probe ile raporla.
- [ ] Üçüncü taraf uygulamaların sandbox özelliği yoksa başlatılmayacağını;
  yalnız açıkça işaretlenmiş geliştirme/trusted profilinin istisna olacağını
  kararlaştır.

## Değişmez güvenlik kuralları

- [ ] `lcl-core`, `lcl-core-android` ve `lcl-rasterd` uygulama sandbox'ının
  dışında kalır; uygulamalar GPU, DRM, Composer veya input aygıtlarına doğrudan
  erişmez.
- [ ] Her uygulama farklı bir gerçek Linux UID/GID ile çalışır; `USER=Rei`
  ortam değişkeni kimlik yerine geçmez.
- [ ] Uygulamalar varsayılan olarak root, `CAP_SYS_ADMIN`, `CAP_SYS_PTRACE`
  veya retained capability olmadan başlatılır.
- [ ] Bir uygulama ancak kullanıcı tarafından doğrulanmış bir elevation
  isteğinden sonra, ayrı ve geçici elevated-app oturumunda ek yetki alabilir.
- [ ] Uygulama manifesti yalnız izin isteyebilir; kendisine izin veremez.
- [ ] Etkin izin `manifest isteği ∩ kullanıcı kararı ∩ sistem politikası`
  olarak hesaplanır.
- [ ] Kullanıcı Ayarları Android SELinux policy'sini veya kernel güvenlik
  sınırını genişletemez.
- [ ] Uygulama izinleri başka uygulamanın verisini ya da sistem aygıtlarını
  doğrudan görünür kılmaz; erişim portal/broker üzerinden sağlanır.
- [ ] Terminal, kullanıcıya kabuk verdiği için üçüncü taraf uygulama profili
  değildir; ayrı bir trusted-user-shell profiliyle çalışır ve root olmaz.

## 1. Bundle ve uygulama kimliği sertleştirmesi

- [ ] `Manifest.json` ayrıştırmasını basit string aramasından gerçek JSON
  ayrıştırıcısına taşı.
- [ ] Manifest şemasına `requestedPermissions` dizisini ekle.
- [ ] Uygulama ID biçimini doğrula ve kurulumda sabit canonical ID olarak sakla.
- [ ] `executable` alanını bundle kökü altında zorunlu kıl; mutlak yol,
  `..` bileşeni ve bundle dışına çıkan symlink'leri reddet.
- [ ] Icon, resource ve executable erişimlerinde race/symlink kaçışını önle.
- [ ] Sistem, kullanıcı ve geliştirme bundle kaynaklarının öncelik ve güven
  kurallarını tanımla.
- [ ] Üçüncü taraf bundle için imza/doğrulama tasarımını belirle; imza yoksa
  yalnız geliştirici modunda kurulum kararını açıkça tanımla.
- [ ] Bundle parser ve registry için geçersiz JSON, path traversal, mutlak
  executable, duplicate ID ve symlink testleri ekle.

## 2. Kimlik ve kalıcı uygulama depolaması

- [ ] LCL'ye ayrılmış UID/GID aralığını tanımla; Android substrate'ında bu
  aralığın Android UID alanıyla çakışmadığını doğrula.
- [ ] `app-id -> uid/gid` eşlemesini root-owned kalıcı registry'de sakla.
- [ ] Her kurulumda kalıcı `Data`, `Cache` ve `Preferences` dizinlerini
  oluştur; `Temporary`yi sandbox başlangıcında tmpfs olarak oluştur.
- [ ] Uygulama özel dizinlerini ilgili UID/GID sahibi ve `0700` moduyla oluştur.
- [ ] Rootfs üreticisindeki genel `0755` permission geçişinin uygulama özel
  dizinleri gevşetmesini engelle.
- [ ] Uygulama kaldırma, veri saklama ve veri silme kararlarını ayrı işlemler
  olarak tasarla.
- [ ] UID/UID registry ve uygulama dizinleri için bozuk sahiplik/izin onarım
  testleri ekle.

## 3. Ayrıcalıklı sandbox launcher

- [ ] `system/security/` altında platformdan bağımsız `SandboxProfile`,
  `SandboxLaunchRequest` ve `SandboxLaunchResult` sözleşmelerini oluştur.
- [ ] Küçük, root çalışan `lcl-sandboxd` servisinin sorumluluğunu yalnız
  doğrulanmış app ID çözümü, sandbox kurulumu ve child reaping ile sınırla.
- [ ] `lcl-sessiond`'yi uygulama yaşam döngüsü otoritesi olarak koru; doğrudan
  `fork/exec` yerine sandboxd'ye doğrulanmış spawn isteği göndersin.
- [ ] Sandboxd'nin istemciden keyfî executable yolu, UID, mount yolu veya
  capability maskesi kabul etmesini engelle.
- [ ] Sessiond ile sandboxd arasındaki IPC'yi owner-only socketpair veya
  doğrulanmış Unix socket üzerinden kur.
- [ ] Launch request'e app ID, instance ID, güvenilir bundle referansı ve
  etkin profil digest'ini bağla.
- [ ] Sandboxd crash/restart durumunda canlı uygulama process group'larının
  nasıl ele alınacağını belirle.
- [ ] Child başlamadan önce group temizleme, `setresgid`, `setresuid`,
  capability drop ve environment temizleme sırasını uygula.
- [ ] `LD_PRELOAD`, tehlikeli `LD_*`, miras alınmış FD'ler ve shell environment
  değerlerini denylist/allowlist ile temizle.
- [ ] Sandbox launcher için UID düşmesi, capability sıfırlama ve kötü niyetli
  spawn isteği testleri ekle.

## 3.1. Uygulama elevation ve `lcl-sudo`

- [ ] Root çalışan `lcl-admind` servisini sandboxd'den ayrı tut; sorumluluğunu
  yalnız admin yetkisi değerlendirme, elevation child başlatma ve denetim
  kaydı ile sınırla.
- [ ] Manifest şemasına uygulamanın elevation isteyebildiğini bildiren
  `admin.elevation` isteğini ekle; bu alan otomatik grant sayılmaz.
- [ ] `ElevationRequest` sözleşmesine app ID, bundle identity/hash, neden,
  istenen kapsam, hedef helper/komut ve instance kimliğini bağla.
- [ ] Elevation isteğini yalnız görünür ve kullanıcı-etkileşimli uygulama
  bağlamından kabul et; arka plan uygulaması kendiliğinden prompt açamasın.
- [ ] Onay promptunu yalnız trusted shell/system surface üzerinde göster;
  uygulama kullanıcı parolasını, PIN'ini veya admin token'ını görmesin.
- [ ] Önce dar kapsamlı elevation uygula: kurulum, güncelleme, sistem ayarı
  veya uygulamanın imzalı privileged helper'ı gibi belirli bir işlem.
- [ ] Kullanıcı açıkça isterse uygulamayı yeniden başlatan geçici
  `elevated-app` oturumu ekle; bu oturumun mount namespace'i ve runtime
  endpoint'leri normal uygulamadan ayrı olsun.
- [ ] Elevated-app oturumunda dahi uygulamaya compositor/rasterd aygıtları,
  Android host `/data`, ham block device veya sınırsız kernel capability verme.
- [ ] Elevation grant'ini tek işlem, tek instance veya zaman sınırlı oturum
  olarak modelle; uygulamanın diske kalıcı root token yazmasını engelle.
- [ ] Elevation revoke, uygulama kapanışı veya session kilidi durumunda
  elevated child'ı ve ona bağlı grant'leri sonlandır.
- [ ] Terminaldeki `sudo <komut>` söz dizimini aynı admin servisine bağla;
  interaktif root shell yalnız trusted Terminal ve açık kullanıcı onayıyla
  oluşturulabilsin.
- [ ] Elevation kararları için app kimliği, kapsam, zaman, kullanıcı kararı ve
  sonuç içeren değiştirilemez admin audit kaydı oluştur.
- [ ] Prompt olmadan elevation, başka app token'ı kullanımı, environment/FD
  sızıntısı, revoke ve elevated-child crash testlerini ekle.

## 4. Uygulama başına filesystem görünümü

- [ ] Her uygulama için private mount namespace oluştur ve mount propagation'ı
  recursive private yap.
- [ ] Yeni sandbox kökünü tmpfs veya eşdeğer güvenli kök üzerinde kur.
- [ ] `/App` altına yalnız ilgili `.app` bundle'ını read-only bind mount et.
- [ ] `/System`, gerekli dinamik kütüphaneler, fontlar ve runtime dosyalarını
  read-only bind mount et.
- [ ] `/Data`, `/Cache`, `/Preferences` ve `/Temporary`yi uygulamanın özel
  depolamasına bağla; `HOME` ve `TMPDIR`yi buna göre ayarla.
- [ ] `/proc`yi private PID namespace'e göre, `/dev`yi ise minimum cihaz
  kümesiyle kur.
- [ ] Uygulamaya `/dev/dri`, `/dev/input`, block device, Android Binder ve
  host `/data` görünürlüğü verme.
- [ ] `/Runtime` için bütün ortak runtime ağacını bind etmek yerine uygulama
  başına dar ve authenticated endpoint görünümü oluştur.
- [ ] Sistem, bundle ve diğer uygulamaların verisine yazmayı engelleyen mount
  ve DAC testleri ekle.
- [ ] Başka uygulamanın `Data` dizinine erişimin `EACCES` veya `ENOENT` ile
  sonuçlandığını QEMU ve Android'de doğrula.

## 5. Compositor, rasterd ve session IPC uyarlaması

- [ ] Uygulama UID'leri farklı olduğunda compositor/rasterd socket erişiminin
  nasıl yetkilendirileceğini tanımla.
- [ ] `SO_PEERCRED` kimliğini sandbox registry'deki app ID ve instance ID ile
  eşleştir.
- [ ] Mevcut surface producer grant mekanizmasını yeni process kimliğiyle
  doğrula; grant'in başka uygulama tarafından kullanılamamasını test et.
- [ ] Uygulama erişebilen compositor/raster endpoint'lerini sessiond/admin
  endpointlerinden ayır.
- [ ] Uygulamaların doğrudan sessiond üzerinden keyfî başka uygulama
  başlatmasını engelle; gerekiyorsa sınırlı `open`/launch portalı tasarla.
- [ ] Compositor socket'in mevcut `0600` modelini, benzersiz app UID'leriyle
  uyumlu ve peer-authenticated bir modele geçir.
- [ ] Socket yeniden başlatmalarında stale bind mount oluşturmayan per-instance
  runtime endpoint yaşam döngüsünü oluştur.
- [ ] Normal pencere açma, popup, raster producer grant ve process exit
  entegrasyon testlerini sandbox altında çalıştır.

## 6. Kernel policy katmanları

- [ ] Sandbox child'ında `PR_SET_NO_NEW_PRIVS` uygula.
- [ ] C++ ve JavaScript runtime'ları için ayrı, mimari-doğrulanmış seccomp
  allowlist'leri oluştur.
- [ ] Seccomp filtrelerinde mimari kontrolü uygula; `ptrace`, `mount`,
  `bpf`, `kexec`, kernel module ve privilege-escalation yollarını reddet.
- [ ] Seccomp profilini dinamik loader ve gerekli normal uygulama davranışları
  ile trace ederek daralt; filter'i geniş bir production allowlist'e dönüştürme.
- [ ] Landlock mevcutsa uygulamanın read/write filesystem köklerine ikinci
  kısıtlama katmanını uygula.
- [ ] Landlock ABI yoksa profilin güvenlik seviyesini açıkça raporla; üçüncü
  taraf uygulamalar için kabul/fail-closed kararını uygula.
- [ ] Network izni olmayan uygulamaya boş network namespace uygula.
- [ ] Network izni olan uygulamalar için yalnız istemci bağlantısına izin veren
  net policy/broker kararını tanımla; dinamik izin değişiminde yeniden başlatma
  gereksinimini belgeleyin.
- [ ] cgroup v2 ile her instance için `memory.max`, `pids.max`, CPU ağırlığı ve
  gerekirse IO sınırları uygula.
- [ ] Yasak syscall, fork bomb, network-deny, cihaz erişimi ve memory limiti
  negatif testleri ekle.

## 7. Android substrate enforcement

- [ ] Mevcut global `setenforce 0` geliştirme yolunu production başlangıç
  akışından çıkar.
- [ ] Android init policy'sinde `lcl_bootstrap`, `lcl_core_android`,
  `lcl_rasterd`, `lcl_sandboxd`, `lcl_admind`, `lcl_app` ve
  `lcl_elevated_app` için ayrı domainleri tanımla.
- [ ] Bootstrap domain'ine rootfs attach için gereken en dar mount/chroot
  yetkilerini ver; bootstrap tamamlanınca ayrıcalıklı iş yapmamasını sağla.
- [ ] Compositor domain'ine yalnız Composer/Binder/graphics ihtiyaçlarını ver.
- [ ] Rasterd domain'ine yalnız native-buffer/graphics ihtiyaçlarını ver.
- [ ] Sandboxd domain'ine yalnız sandbox kurulumu için gereken yetkileri ver;
  Android user data, input ve network yönetim erişimi verme.
- [ ] Uygulama domain'ini capability'siz, aygıtsız ve default-deny tanımla.
- [ ] Elevated-app domain'ini normal uygulamadan ayrı tanımla; yalnız
  kullanıcının onayladığı LCL yönetim kapsamına erişim ver, Android substrate
  veya diğer uygulama verilerine genel erişim verme.
- [ ] Canonical glibc sandboxd'nin Android SELinux domainine güvenli geçişini
  substrate wrapper veya init service üzerinden tasarla.
- [ ] Android cgroup yerleştirmesini canonical ABI'ye sızdırmadan substrate
  bridge/task-profile katmanında uygula.
- [ ] Android'de user namespace'e bağımlı olmadan root sandboxd'nin gerçek
  rezerve UID'lerle namespace kurabildiğini doğrula.
- [ ] Geliştirme için yalnız ilgili LCL domain'ini geçici permissive yap;
  global permissive kullanma.
- [ ] SELinux denial loglarını analiz et, policy'yi daralt ve enforcing AVD
  ile fiziksel hedef doğrulamasını ayrı ayrı kaydet.

## 8. İzin veritabanı ve policy değerlendirme

- [ ] Root-owned, atomic güncellenen `PermissionStore` tasarla.
- [ ] İzin kararını app ID, bundle signing identity/hash, kullanıcı ve izin
  sürümü ile bağla.
- [ ] Varsayılanı deny olarak ayarla.
- [ ] İlk sistem uygulamaları için read-only, imzaya bağlı statik profiller
  oluştur.
- [ ] `network.client`, `files.user-selected`, `files.documents.read`,
  `files.documents.write`, `clipboard.read`, `camera`, `microphone` ve
  `notifications` izinlerinin anlamlarını belirle.
- [ ] `admin.elevation` iznini "elevation isteyebilir" olarak tanımla; her
  gerçek elevation için ayrıca görünür kullanıcı onayı gerektir.
- [ ] İzinlerin launch-time mı, broker-time mı değerlendirileceğini tek tek
  belirle.
- [ ] Grant/revoke ve bundle güncellemesi sonrası eski grant'in geçersizleşme
  kurallarını tanımla.
- [ ] PermissionStore için bozuk kayıt, downgrade, bundle değişimi ve
  yetkisiz yazma testleri ekle.

## 9. Portal ve güvenilir shell promptları

- [ ] File portalı tasarla: shell dosya seçer, seçilen FD uygulamaya aktarılır;
  uygulamaya geniş dizin yolu verilmez.
- [ ] Clipboard, kamera, mikrofon, bildirim ve launch işlemleri için ayrı
  broker/portal sözleşmelerini tasarla.
- [ ] Uygulamanın izin promptu taklit etmesini önlemek için promptu yalnız
  trusted shell/system surface üzerinde göster.
- [ ] Promptta uygulama adı, doğrulanmış yayıncı/kimlik, istenen izin,
  erişim kapsamı ve kararın süresi göster.
- [ ] Prompt kararını PermissionStore'a atomik olarak yaz.
- [ ] Portal istemcisini app UID/instance kimliğiyle yetkilendir.
- [ ] Portalın FD aktarımı, revoke davranışı ve uygulama kapanışı testlerini
  ekle.

## 10. Ayarlar uygulaması

- [ ] Ayarlar uygulamasını sandbox mekanizmasının ilk bağımlılığı yapma.
- [ ] Trusted system Settings profili oluştur; root yerine sınırlı admin IPC
  capability'si kullan.
- [ ] Uygulama başına etkin izinleri, izin kaynağını ve son karar zamanını
  listeleyen ekranı tasarla.
- [ ] Grant/revoke kontrollerini PermissionStore admin API'sine bağla.
- [ ] Yeniden başlatma gerektiren izin değişikliklerini açıkça göster.
- [ ] Uygulama elevation geçmişini, aktif elevated oturumları ve revoke
  kontrolünü göster.
- [ ] Uygulama verisini silme, izinleri sıfırlama ve uygulamayı kaldırma
  eylemlerini birbirinden ayır.
- [ ] Settings admin API'sinde `SO_PEERCRED`, trusted app identity ve yetki
  kontrol testleri ekle.

## 11. Paketleme, güncelleme ve geliştirici modu

- [ ] Sistem uygulamalarını read-only rootfs alanında paketle.
- [ ] Kullanıcı uygulama kurulumu için transactional staging, doğrulama,
  signature kontrolü ve atomik registry güncellemesi uygula.
- [ ] Geliştirici modunu görünür, kalıcı olmayan ve varsayılan kapalı yap.
- [ ] Geliştirici modunda sandbox bypass yerine açıkça işaretlenmiş daha geniş
  bir profil kullan; root çalıştırma vermeyi yasakla.
- [ ] Uygulama güncellemesinde UID, Data dizini ve grant migration davranışını
  tanımla.
- [ ] Rollback ve yarım kalmış kurulum temizliği testlerini ekle.

## 12. Doğrulama ve kabul kriterleri

- [ ] Her uygulama için UID/GID, capability, namespace inode'ları, cgroup
  yolu ve etkin profil digest'ini yalnız admin diagnostic aracıyla raporla.
- [ ] Host unit testleri: manifest, permission store, profile çözümü,
  UID registry ve IPC kimlik kontrolü.
- [ ] Host integration testleri: iki uygulamanın karşılıklı veri erişimi,
  private runtime endpoint ve process cleanup.
- [ ] QEMU acceptance: uygulama root değildir; diğer app Data görünmez;
  `/System` yazılamaz; yasak network/aygıt erişimi başarısız olur.
- [ ] Android enforcing acceptance: SELinux enforcing kalır; aynı negatif
  erişim testleri canonical rootfs içinde geçer.
- [ ] Android compositor/rasterd acceptance: sandboxed uygulama pencere açar,
  rasterd producer grant alır ve AHardwareBuffer sunumu bozulmaz.
- [ ] Terminal acceptance: kullanıcı UID'sinde PTY/bash çalışır; root shell
  açılmaz; kendi trusted profilinin kapsamı doğrulanır.
- [ ] Stress acceptance: process crash, socket restart, permission revoke,
  cgroup OOM ve hızlı launch/exit döngülerinde kaynak sızıntısı olmaz.
- [ ] Her kabul maddesi için kanıt seviyesini ayrı yaz: static scan, host test,
  QEMU run, Android run veya kullanıcı-gözlemi.

## Tamamlanma tanımı

- [ ] Üçüncü taraf native veya JavaScript uygulaması root olmadan, farklı UID
  ile ve private filesystem görünümünde başlatılır.
- [ ] Uygulama başka uygulamanın verisini, system dosyalarını veya aygıtlarını
  doğrudan okuyamaz/yazamaz.
- [ ] Android hedefinde SELinux enforcing kalırken LCL uygulaması çalışır.
- [ ] Kullanıcı onayıyla normal uygulama dar kapsamlı veya full elevated-app
  oturumuna geçebilir; onaysız uygulama elevation alamaz.
- [ ] Compositor/rasterd retained presentation sözleşmesi ve Same-Binary
  Invariant korunur.
- [ ] Kullanıcı izinleri shell promptu ve Ayarlar uygulamasından değiştirilebilir
  fakat kernel/SELinux güvenlik tavanını genişletemez.
