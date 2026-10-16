using System;
using System.Collections.Generic;
using System.Linq;
using PRODUCT_NAMESPACE;
internal static class DriverSelectionsRegression {
 static void Check(bool value,string message) {if(!value)throw new Exception(message);}
 [STAThread] static int Main(string[] fixtures) {
  var saved=new Dictionary<string,int>();
  Func<string,int?> read=name=>saved.ContainsKey(name)?(int?)saved[name]:null;
  Action<string,bool> write=(name,value)=>saved[name]=value?1:0;
  var fresh=new InstallerDriverSelections(read,write);
  Check(fresh.DisplayEnabled && fresh.GamepadEnabled,"clean and legacy missing values default on");
  fresh.SelectDisplay(false);fresh.SelectGamepad(false);
  var canceledReopen=new InstallerDriverSelections(read,write);
  Check(!canceledReopen.DisplayEnabled && !canceledReopen.GamepadEnabled,"cancel/reopen retains explicit optouts");
  canceledReopen.SelectDisplay(true);
  var upgrade=new InstallerDriverSelections(read,write);
  Check(upgrade.DisplayEnabled && !upgrade.GamepadEnabled,"upgrade and repair restore separate choices");
  upgrade.SelectGamepad(true);upgrade.SelectDisplay(false);upgrade.SelectDisplay(true);
  Check(new InstallerDriverSelections(read,write).GamepadEnabled,"back/forward changes retain other selection");
  Check(InstallerDriverSelectionStore.Parse("0")==0 && InstallerDriverSelectionStore.Parse("1")==1 && !InstallerDriverSelectionStore.Parse("unknown").HasValue,"stored string migration and malformed defaults");
  var args=InstallerArguments.Parse(new[]{"--internal-elevated-install","--internal-install-display-drivers","0"});
  var options=InstallerRunner.BuildDriverSelectionArguments(args,true,false).ToArray();
  Check(options.Contains("INSTALL_DISPLAY_DRIVERS=0") && options.Contains("INSTALL_VIRTUAL_DISPLAY_DRIVER=1") && options.Contains("INSTALL_VIRTUAL_GAMEPAD_DRIVER=0"),"optouts map to MSI and preserve backend");
  args=InstallerArguments.Parse(new[]{"--ui"});
  Check(InstallerRunner.BuildDriverSelectionArguments(args,true,true).Contains("INSTALL_DISPLAY_DRIVERS=1"),"missing internal option defaults on");
  var payload=new InstallerRunner.PayloadMsiInfo {HasLocalGamepadCertificate=true,LocalGamepadCertificateThumbprint="7F1868BF86698A8674F40E686B02C8C702BD3B88"};
  InstallerRunner.ApplySelectedGamepadTrust(args,payload,true);
  Check(args.AllowLocalGamepadCertificateTrust && args.ConsentCertificateThumbprint==payload.LocalGamepadCertificateThumbprint,"selected verified beta binds exact trust without second checkbox");
  InstallerRunner.ApplySelectedGamepadTrust(args,payload,false);
  Check(!args.AllowLocalGamepadCertificateTrust && args.ConsentCertificateThumbprint=="","optout adds no trust");
  payload.LocalGamepadCertificateThumbprint=new string('A',40);
  InstallerRunner.ApplySelectedGamepadTrust(args,payload,true);
  Check(!args.AllowLocalGamepadCertificateTrust,"wrong identity cannot authorize trust");
  payload.HasLocalGamepadCertificate=false;
  InstallerRunner.ApplySelectedGamepadTrust(args,payload,true);
  Check(!args.AllowLocalGamepadCertificateTrust,"production signer does not enter beta trust flow");
  if(fixtures.Length!=0) {
   Check(fixtures.Length==4,"bundled, absent, missing and malformed capability fixtures required");
   for(int i=0;i<fixtures.Length;i++) {
    var metadata=InstallerRunner.TryGetPayloadMsiInfo(new InstallerArguments {MsiPathOverride=fixtures[i]});
    Check(metadata!=null,"actual MSI fixture metadata must be readable");
    Check(metadata.HasBundledVirtualGamepadDriver==(i==0),"bundle detection must ignore mutable install selection and fail closed");
    var enabled=new InstallerDriverSelections(name=>null,(name,value)=>{});
    bool install=metadata.HasBundledVirtualGamepadDriver && enabled.GamepadEnabled;
    Check(InstallerRunner.BuildDriverSelectionArguments(args,true,install).Contains("INSTALL_VIRTUAL_GAMEPAD_DRIVER="+(i==0?"1":"0")),"fresh bundle defaults on; absent capability defaults off");
    enabled.SelectGamepad(false);
    install=metadata.HasBundledVirtualGamepadDriver && enabled.GamepadEnabled;
    Check(InstallerRunner.BuildDriverSelectionArguments(args,true,install).Contains("INSTALL_VIRTUAL_GAMEPAD_DRIVER=0"),"explicit optout survives bundled capability");
    InstallerRunner.ApplySelectedGamepadTrust(args,metadata,install);
    Check(!args.AllowLocalGamepadCertificateTrust,"optout cannot authorize certificate trust");
    InstallerRunner.ApplySelectedGamepadTrust(args,metadata,metadata.HasBundledVirtualGamepadDriver);
    Check(args.AllowLocalGamepadCertificateTrust==(i==0),"bundle metadata only permits selected pinned beta certificate");
   }
   Console.WriteLine("4 actual MSI capability fixtures passed without running MSI actions or changing trust.");
  }
  Console.WriteLine("Driver default/persistence/cancel/repair/MSI/elevation/pinned-beta policy regressions passed. No registry, UI, installers or trust changes.");
  return 0;
 }
}
