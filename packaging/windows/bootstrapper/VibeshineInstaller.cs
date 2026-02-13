using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using Microsoft.Win32;
using System.Reflection;
using System.Security.Cryptography;
using System.Security.Principal;
using System.Text;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Documents;
using System.Windows.Input;
using System.Windows.Interop;
using System.Windows.Media;
using System.Windows.Threading;

namespace VibepolloInstaller {
  internal static class BuildFlavor {
#if UNINSTALL_ONLY
    public static readonly bool IsUninstallOnly = true;
#else
    public static readonly bool IsUninstallOnly = false;
#endif
  }

  internal static class Program {
    [STAThread]
    private static int Main(string[] args) {
      if (InstallerArguments.IsHelpRequested(args)) {
        InstallerArguments.WriteHelp();
        return 0;
      }

      var parsed = InstallerArguments.Parse(args);
      if (BuildFlavor.IsUninstallOnly) {
        parsed.UninstallUiRequested = true;
      }
      if (parsed.UninstallUiRequested && !parsed.ShowUi && !parsed.InternalElevatedUninstall) {
        var uninstallResult = InstallerRunner.RunInteractiveUninstall(parsed, false, false);
        if (!string.IsNullOrWhiteSpace(uninstallResult.Message)) {
          Console.WriteLine(uninstallResult.Message);
        }
        return uninstallResult.ExitCode == 1605 ? 0 : uninstallResult.ExitCode;
      }
      if (parsed.InternalElevatedInstall) {
        var installPath = string.IsNullOrWhiteSpace(parsed.InternalInstallPath)
          ? InstallerRunner.DefaultInstallDirectory
          : parsed.InternalInstallPath;
        var internalInstall = InstallerRunner.RunInteractiveInstall(
          parsed,
          installPath,
          parsed.InternalInstallVirtualDisplay,
          parsed.InternalInstallSaveLogs,
          false);
        InstallerRunner.TryWriteInternalInstallResult(parsed.InternalInstallResultPath, internalInstall);
        return internalInstall.ExitCode;
      }

      if (parsed.InternalElevatedUninstall) {
        var internalUninstall = InstallerRunner.RunInteractiveUninstall(
          parsed,
          parsed.InternalUninstallDeleteInstallDir,
          parsed.InternalUninstallRemoveVirtualDisplayDriver,
          false);
        return internalUninstall.ExitCode;
      }

      if (!parsed.ShowUi) {
        var cliResult = InstallerRunner.RunCli(parsed);
        if (!string.IsNullOrWhiteSpace(cliResult.Message)) {
          Console.WriteLine(cliResult.Message);
        }
        return cliResult.ExitCode;
      }

      var app = new Application {
        ShutdownMode = ShutdownMode.OnMainWindowClose
      };
      var window = new InstallerWindow(parsed);
      app.Run(window);
      return window.ProcessExitCode;
    }
  }

  internal sealed class InstallerWindow : Window {
    private readonly InstallerArguments _arguments;
    private readonly Border _installSection;
    private readonly TextBox _installPathTextBox;
    private readonly CheckBox _installVirtualDisplayCheckBox;
    private readonly TextBlock _statusText;
    private readonly TextBlock _statusDetailText;
    private readonly ProgressBar _progressBar;
    private readonly Button _browseButton;
    private readonly Button _continueButton;
    private readonly Button _uninstallButton;
    private readonly Button _licenseButton;
    private readonly Button _closeButton;
    private readonly Button _titleCloseButton;
    private readonly Grid _overlayGrid;
    private readonly Border _overlayAccentBar;
    private readonly TextBlock _overlayTitleText;
    private readonly TextBlock _overlayMessageText;
    private readonly TextBlock _overlayHintText;
    private readonly ProgressBar _overlayAutoCloseProgressBar;
    private readonly StackPanel _overlayContentHost;
    private readonly Button _overlayPrimaryButton;
    private readonly Button _overlaySecondaryButton;
    private TaskCompletionSource<string> _overlayTcs;
    private DispatcherTimer _overlayAutoCloseTimer;
    private int _overlayAutoCloseSecondsRemaining;
    private int _overlayAutoCloseTotalSeconds;
    private DateTime _overlayAutoCloseDeadlineUtc;
    private bool _isBusy;
    private readonly bool _uninstallUiRequested;
    private readonly Brush _statusNormalBrush = new SolidColorBrush(Color.FromRgb(245, 249, 255));
    private readonly Brush _statusBusyBrush = new SolidColorBrush(Color.FromRgb(147, 197, 253));
    private readonly Brush _statusSuccessBrush = new SolidColorBrush(Color.FromRgb(16, 185, 129));
    private readonly Brush _statusWarningBrush = new SolidColorBrush(Color.FromRgb(251, 191, 36));
    private readonly Brush _statusErrorBrush = new SolidColorBrush(Color.FromRgb(255, 178, 196));
    private readonly Version _bundleVersion;
    private readonly InstallerRunner.InstalledProductInfo _installedProduct;
    private readonly InstallerRunner.InstalledProductInfo _legacySunshineProduct;
    private readonly InstallerRunner.LegacySunshineRegistration _legacySunshineRegistration;
    private readonly InstallerRunner.LegacySunshineRegistration _legacyApolloRegistration;
    private readonly InstallerRunner.PayloadMsiInfo _payloadMsiInfo;
    private readonly string _licenseText;
    private static readonly IntPtr HWND_TOPMOST = new IntPtr(-1);
    private static readonly IntPtr HWND_NOTOPMOST = new IntPtr(-2);
    private const uint SWP_NOMOVE = 0x0002;
    private const uint SWP_NOSIZE = 0x0001;
    private const uint SWP_NOACTIVATE = 0x0010;
    private const int SW_RESTORE = 9;

    public int ProcessExitCode { get; private set; }

    public InstallerWindow(InstallerArguments arguments) {
      _arguments = arguments;
      _bundleVersion = Assembly.GetExecutingAssembly().GetName().Version ?? new Version(0, 0, 0, 0);
      _payloadMsiInfo = InstallerRunner.TryGetPayloadMsiInfo(arguments);
      _licenseText = LoadEmbeddedLicenseText();
      _installedProduct = InstallerRunner.GetInstalledVibepolloProduct();
      _legacySunshineProduct = InstallerRunner.GetInstalledSunshineProduct();
      _legacySunshineRegistration = InstallerRunner.GetLegacySunshineRegistration();
      _legacyApolloRegistration = InstallerRunner.GetLegacyApolloRegistration();
      _uninstallUiRequested = BuildFlavor.IsUninstallOnly || arguments.UninstallUiRequested;
      var showInstallOptions = !BuildFlavor.IsUninstallOnly && _installedProduct == null;
      var displayVersion = GetTargetVersionText();
      Title = (BuildFlavor.IsUninstallOnly ? "Vibepollo Uninstaller v" : "Vibepollo Installer v") + displayVersion;
      Width = 720;
      Height = showInstallOptions ? 560 : 460;
      MinWidth = 690;
      MinHeight = showInstallOptions ? 520 : 430;
      WindowStartupLocation = WindowStartupLocation.CenterScreen;
      ResizeMode = ResizeMode.CanMinimize;
      WindowStyle = WindowStyle.None;
      AllowsTransparency = false;
      Background = CreateBackgroundBrush();
      FontFamily = new FontFamily("Segoe UI");

      var root = new Grid {
        Background = new SolidColorBrush(Color.FromRgb(6, 10, 24))
      };
      root.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
      root.RowDefinitions.Add(new RowDefinition { Height = new GridLength(1, GridUnitType.Star) });
      Content = root;

      var titleBar = new Border {
        Height = 36,
        Background = new SolidColorBrush(Color.FromRgb(10, 16, 30)),
        BorderBrush = new SolidColorBrush(Color.FromRgb(52, 64, 94)),
        BorderThickness = new Thickness(0, 0, 0, 1)
      };
      titleBar.MouseLeftButtonDown += TitleBarMouseLeftButtonDown;
      Grid.SetRow(titleBar, 0);
      root.Children.Add(titleBar);

      var titleGrid = new Grid();
      titleGrid.ColumnDefinitions.Add(new ColumnDefinition());
      titleGrid.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
      titleGrid.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
      titleBar.Child = titleGrid;

      var titleBarText = new TextBlock {
        Text = Title,
        Foreground = new SolidColorBrush(Color.FromRgb(224, 236, 255)),
        FontSize = 12,
        Margin = new Thickness(14, 0, 0, 0),
        VerticalAlignment = VerticalAlignment.Center
      };
      titleGrid.Children.Add(titleBarText);

      var titleMinimizeButton = new Button {
        Content = "−",
        Width = 42,
        Height = 28,
        Margin = new Thickness(0, 4, 0, 4),
        Background = Brushes.Transparent,
        Foreground = new SolidColorBrush(Color.FromRgb(224, 236, 255)),
        BorderBrush = Brushes.Transparent,
        BorderThickness = new Thickness(0),
        FontSize = 16,
        ToolTip = "Minimize"
      };
      titleMinimizeButton.MouseEnter += TitleMinimizeMouseEnter;
      titleMinimizeButton.MouseLeave += TitleButtonMouseLeave;
      titleMinimizeButton.Click += MinimizeClicked;
      ApplyFlatButtonTemplate(titleMinimizeButton, 4);
      Grid.SetColumn(titleMinimizeButton, 1);
      titleGrid.Children.Add(titleMinimizeButton);

      _titleCloseButton = new Button {
        Content = "✕",
        Width = 42,
        Height = 28,
        Margin = new Thickness(2, 4, 8, 4),
        Background = Brushes.Transparent,
        Foreground = new SolidColorBrush(Color.FromRgb(224, 236, 255)),
        BorderBrush = Brushes.Transparent,
        BorderThickness = new Thickness(0),
        FontSize = 12,
        ToolTip = "Close"
      };
      _titleCloseButton.MouseEnter += TitleCloseMouseEnter;
      _titleCloseButton.MouseLeave += TitleButtonMouseLeave;
      _titleCloseButton.Click += (sender, eventArgs) => Close();
      ApplyFlatButtonTemplate(_titleCloseButton, 4);
      Grid.SetColumn(_titleCloseButton, 2);
      titleGrid.Children.Add(_titleCloseButton);

      var card = new Border {
        CornerRadius = new CornerRadius(18),
        Margin = new Thickness(20, 10, 20, 12),
        Padding = new Thickness(20),
        Background = new SolidColorBrush(Color.FromArgb(238, 14, 20, 36)),
        BorderBrush = new SolidColorBrush(Color.FromArgb(145, 99, 102, 241)),
        BorderThickness = new Thickness(1.2),
        VerticalAlignment = VerticalAlignment.Top
      };
      Grid.SetRow(card, 1);
      root.Children.Add(card);

      _overlayGrid = new Grid {
        Background = new SolidColorBrush(Color.FromArgb(172, 4, 8, 18)),
        Visibility = Visibility.Collapsed
      };
      Panel.SetZIndex(_overlayGrid, 50);
      Grid.SetRowSpan(_overlayGrid, 2);
      root.Children.Add(_overlayGrid);

      var overlayCard = new Border {
        CornerRadius = new CornerRadius(16),
        Padding = new Thickness(20),
        Background = new SolidColorBrush(Color.FromRgb(10, 16, 32)),
        BorderBrush = new SolidColorBrush(Color.FromRgb(86, 102, 146)),
        BorderThickness = new Thickness(1.2),
        HorizontalAlignment = HorizontalAlignment.Center,
        VerticalAlignment = VerticalAlignment.Center,
        Width = 540,
        MaxHeight = 390
      };
      _overlayGrid.Children.Add(overlayCard);

      var overlayLayout = new Grid();
      overlayLayout.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
      overlayLayout.RowDefinitions.Add(new RowDefinition { Height = new GridLength(1, GridUnitType.Star) });
      overlayLayout.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
      overlayCard.Child = overlayLayout;

      _overlayAccentBar = new Border {
        Height = 4,
        CornerRadius = new CornerRadius(3),
        Margin = new Thickness(0, 0, 0, 14),
        Background = new SolidColorBrush(Color.FromRgb(99, 102, 241))
      };
      Grid.SetRow(_overlayAccentBar, 0);
      overlayLayout.Children.Add(_overlayAccentBar);

      var overlayBodyScroll = new ScrollViewer {
        VerticalScrollBarVisibility = ScrollBarVisibility.Auto,
        HorizontalScrollBarVisibility = ScrollBarVisibility.Disabled,
        Margin = new Thickness(0, 0, 0, 12)
      };
      Grid.SetRow(overlayBodyScroll, 1);
      overlayLayout.Children.Add(overlayBodyScroll);

      var overlayStack = new StackPanel {
        Orientation = Orientation.Vertical
      };
      overlayBodyScroll.Content = overlayStack;

      _overlayTitleText = new TextBlock {
        FontSize = 17,
        FontWeight = FontWeights.SemiBold,
        Foreground = new SolidColorBrush(Color.FromRgb(245, 249, 255)),
        Margin = new Thickness(0, 0, 0, 10),
        TextWrapping = TextWrapping.Wrap
      };
      overlayStack.Children.Add(_overlayTitleText);

      _overlayMessageText = new TextBlock {
        FontSize = 13,
        Foreground = new SolidColorBrush(Color.FromRgb(210, 222, 242)),
        Margin = new Thickness(0, 0, 0, 10),
        LineHeight = 19,
        TextWrapping = TextWrapping.Wrap
      };
      overlayStack.Children.Add(_overlayMessageText);

      _overlayHintText = new TextBlock {
        FontSize = 12,
        Foreground = new SolidColorBrush(Color.FromRgb(165, 180, 252)),
        Margin = new Thickness(0, 0, 0, 8),
        Visibility = Visibility.Collapsed,
        TextWrapping = TextWrapping.Wrap
      };
      overlayStack.Children.Add(_overlayHintText);

      _overlayAutoCloseProgressBar = new ProgressBar {
        Height = 4,
        Minimum = 0,
        Maximum = 5,
        Value = 0,
        Visibility = Visibility.Collapsed,
        Margin = new Thickness(2, 0, 2, 12),
        Foreground = new SolidColorBrush(Color.FromRgb(99, 102, 241)),
        Background = new SolidColorBrush(Color.FromRgb(24, 34, 58)),
        BorderThickness = new Thickness(0)
      };
      overlayStack.Children.Add(_overlayAutoCloseProgressBar);

      _overlayContentHost = new StackPanel {
        Orientation = Orientation.Vertical,
        Margin = new Thickness(0, 0, 0, 12)
      };
      overlayStack.Children.Add(_overlayContentHost);

      var overlayButtons = new Grid {
        Margin = new Thickness(0, 6, 0, 0)
      };
      overlayButtons.ColumnDefinitions.Add(new ColumnDefinition());
      overlayButtons.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
      overlayButtons.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
      Grid.SetRow(overlayButtons, 2);
      overlayLayout.Children.Add(overlayButtons);

      _overlaySecondaryButton = new Button {
        Content = "Cancel",
        Height = 38,
        MinWidth = 96,
        Margin = new Thickness(0, 0, 10, 0),
        Padding = new Thickness(16, 0, 16, 0),
        FontWeight = FontWeights.SemiBold,
        Background = new SolidColorBrush(Color.FromRgb(16, 24, 42)),
        Foreground = new SolidColorBrush(Color.FromRgb(232, 239, 253)),
        BorderBrush = new SolidColorBrush(Color.FromRgb(82, 96, 141))
      };
      _overlaySecondaryButton.Click += (sender, eventArgs) => ResolveOverlay("secondary");
      ApplyFlatButtonTemplate(_overlaySecondaryButton, 8);
      Grid.SetColumn(_overlaySecondaryButton, 1);
      overlayButtons.Children.Add(_overlaySecondaryButton);

      _overlayPrimaryButton = new Button {
        Content = "OK",
        Height = 38,
        MinWidth = 108,
        Padding = new Thickness(16, 0, 16, 0),
        FontWeight = FontWeights.SemiBold,
        Background = new SolidColorBrush(Color.FromRgb(99, 102, 241)),
        Foreground = new SolidColorBrush(Color.FromRgb(245, 249, 255)),
        BorderBrush = new SolidColorBrush(Color.FromRgb(165, 180, 252))
      };
      _overlayPrimaryButton.Click += (sender, eventArgs) => ResolveOverlay("primary");
      ApplyFlatButtonTemplate(_overlayPrimaryButton, 8);
      Grid.SetColumn(_overlayPrimaryButton, 2);
      overlayButtons.Children.Add(_overlayPrimaryButton);

      var cardGrid = new Grid();
      cardGrid.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
      cardGrid.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
      card.Child = cardGrid;

      var contentStack = new StackPanel {
        Orientation = Orientation.Vertical
      };
      Grid.SetRow(contentStack, 0);
      cardGrid.Children.Add(contentStack);

      _installSection = new Border {
        CornerRadius = new CornerRadius(10),
        Padding = new Thickness(16),
        Margin = new Thickness(0, 0, 0, 10),
        Background = new SolidColorBrush(Color.FromArgb(44, 99, 102, 241)),
        BorderBrush = new SolidColorBrush(Color.FromArgb(112, 128, 133, 255)),
        BorderThickness = new Thickness(1)
      };
      contentStack.Children.Add(_installSection);

      var installStack = new StackPanel {
        Orientation = Orientation.Vertical
      };
      _installSection.Child = installStack;

      installStack.Children.Add(new TextBlock {
        Text = "Install Location",
        FontSize = 14,
        FontWeight = FontWeights.SemiBold,
        Foreground = Brushes.White,
        Margin = new Thickness(0, 0, 0, 4)
      });

      installStack.Children.Add(new TextBlock {
        Text = "Choose where Vibepollo will be installed. The default is recommended.",
        FontSize = 12.5,
        Foreground = new SolidColorBrush(Color.FromRgb(209, 222, 241)),
        Margin = new Thickness(0, 0, 0, 10),
        TextWrapping = TextWrapping.Wrap
      });

      var pathGrid = new Grid();
      pathGrid.ColumnDefinitions.Add(new ColumnDefinition());
      pathGrid.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
      installStack.Children.Add(pathGrid);

      _installPathTextBox = new TextBox {
        FontSize = 13,
        Height = 36,
        Padding = new Thickness(10, 6, 10, 6),
        VerticalContentAlignment = VerticalAlignment.Center,
        Text = InstallerRunner.DefaultInstallDirectory,
        Background = new SolidColorBrush(Color.FromRgb(10, 16, 30)),
        Foreground = new SolidColorBrush(Color.FromRgb(245, 249, 255)),
        BorderBrush = new SolidColorBrush(Color.FromRgb(96, 111, 171)),
        CaretBrush = new SolidColorBrush(Color.FromRgb(245, 249, 255)),
        ToolTip = "Used when installing or updating Vibepollo"
      };
      pathGrid.Children.Add(_installPathTextBox);

      _browseButton = new Button {
        Content = "_Browse...",
        Margin = new Thickness(10, 0, 0, 0),
        MinWidth = 104,
        Height = 36,
        Padding = new Thickness(16, 0, 16, 0),
        FontWeight = FontWeights.SemiBold,
        Background = new SolidColorBrush(Color.FromRgb(16, 24, 42)),
        Foreground = new SolidColorBrush(Color.FromRgb(232, 239, 253)),
        BorderBrush = new SolidColorBrush(Color.FromRgb(82, 96, 141)),
        ToolTip = "Choose a different install location"
      };
      _browseButton.Click += BrowseClicked;
      ApplyFlatButtonTemplate(_browseButton, 7);
      Grid.SetColumn(_browseButton, 1);
      pathGrid.Children.Add(_browseButton);

      _installVirtualDisplayCheckBox = new CheckBox {
        Content = "Install virtual display driver (SudoVDA)",
        FontSize = 13,
        Foreground = new SolidColorBrush(Color.FromRgb(226, 235, 250)),
        Margin = new Thickness(0, 12, 0, 4),
        IsChecked = true,
        ToolTip = "Recommended for headless streaming or virtual monitor setups"
      };
      installStack.Children.Add(_installVirtualDisplayCheckBox);

      installStack.Children.Add(new TextBlock {
        Text = "Disable only if you already use another virtual display driver or do not need virtual monitors.",
        FontSize = 12,
        Foreground = new SolidColorBrush(Color.FromRgb(190, 208, 236)),
        Margin = new Thickness(24, 0, 0, 0),
        TextWrapping = TextWrapping.Wrap
      });

      var tipsSection = new Border {
        CornerRadius = new CornerRadius(10),
        Padding = new Thickness(16),
        Margin = new Thickness(0, 0, 0, 10),
        Background = new SolidColorBrush(Color.FromArgb(34, 56, 189, 248)),
        BorderBrush = new SolidColorBrush(Color.FromArgb(92, 99, 157, 219)),
        BorderThickness = new Thickness(1)
      };
      contentStack.Children.Add(tipsSection);

      var tipsStack = new StackPanel {
        Orientation = Orientation.Vertical
      };
      tipsSection.Child = tipsStack;

      tipsStack.Children.Add(new TextBlock {
        Text = "Quick Tips",
        FontSize = 14,
        FontWeight = FontWeights.SemiBold,
        Foreground = Brushes.White,
        Margin = new Thickness(0, 0, 0, 4)
      });

      tipsStack.Children.Add(new TextBlock {
        Text = "You can install or upgrade Vibepollo while actively streaming. No system restart is required. "
          + "After you click Install or Upgrade, the current streaming session will end, then you can usually "
          + "start streaming again after about 1–2 minutes without issues.",
        FontSize = 12.5,
        Foreground = new SolidColorBrush(Color.FromRgb(211, 220, 246)),
        Margin = new Thickness(0, 0, 0, 10),
        TextWrapping = TextWrapping.Wrap
      });

      tipsStack.Children.Add(new TextBlock {
        Text = "You can also install from an SSH session on this host (run in an elevated shell):",
        FontSize = 13,
        Foreground = new SolidColorBrush(Color.FromRgb(203, 219, 241)),
        Margin = new Thickness(0, 0, 0, 6),
        TextWrapping = TextWrapping.Wrap
      });

      tipsStack.Children.Add(new TextBox {
        Text = "VibepolloSetup.exe /qn /norestart",
        IsReadOnly = true,
        FontFamily = new FontFamily("Consolas"),
        FontSize = 12.5,
        Margin = new Thickness(0, 0, 0, 8),
        Padding = new Thickness(10, 8, 10, 8),
        Background = new SolidColorBrush(Color.FromRgb(16, 24, 42)),
        Foreground = new SolidColorBrush(Color.FromRgb(226, 235, 250)),
        BorderBrush = new SolidColorBrush(Color.FromRgb(82, 96, 141)),
        CaretBrush = new SolidColorBrush(Color.FromRgb(226, 235, 250))
      });

      tipsStack.Children.Add(new TextBlock {
        Text = "Click the buttons below to proceed.",
        FontSize = 12.5,
        Foreground = new SolidColorBrush(Color.FromRgb(211, 220, 246)),
        Margin = new Thickness(0, 0, 0, 0),
        TextWrapping = TextWrapping.Wrap
      });

      var divider = new System.Windows.Shapes.Rectangle {
        Height = 1,
        Fill = new SolidColorBrush(Color.FromArgb(120, 88, 104, 124)),
        Margin = new Thickness(0, 0, 0, 10)
      };
      contentStack.Children.Add(divider);

      var statusCard = new Border {
        CornerRadius = new CornerRadius(10),
        Padding = new Thickness(14, 10, 14, 10),
        Margin = new Thickness(0, 0, 0, 10),
        Background = new SolidColorBrush(Color.FromArgb(38, 56, 189, 248)),
        BorderBrush = new SolidColorBrush(Color.FromArgb(92, 99, 157, 219)),
        BorderThickness = new Thickness(1)
      };
      statusCard.Visibility = Visibility.Collapsed;
      contentStack.Children.Add(statusCard);

      var statusStack = new StackPanel {
        Orientation = Orientation.Vertical
      };
      statusCard.Child = statusStack;

      statusStack.Children.Add(new TextBlock {
        Text = "STATUS",
        FontSize = 11,
        FontWeight = FontWeights.SemiBold,
        Foreground = new SolidColorBrush(Color.FromRgb(176, 207, 238)),
        Margin = new Thickness(0, 0, 0, 2)
      });

      _statusText = new TextBlock {
        FontSize = 14,
        FontWeight = FontWeights.SemiBold,
        Foreground = _statusNormalBrush,
        Margin = new Thickness(0, 0, 0, 2),
        TextWrapping = TextWrapping.Wrap
      };
      statusStack.Children.Add(_statusText);

      _statusDetailText = new TextBlock {
        FontSize = 12.5,
        Foreground = new SolidColorBrush(Color.FromRgb(203, 219, 241)),
        TextWrapping = TextWrapping.Wrap
      };
      statusStack.Children.Add(_statusDetailText);

      var footerGrid = new Grid {
        Margin = new Thickness(0)
      };
      footerGrid.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
      footerGrid.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
      Grid.SetRow(footerGrid, 1);
      cardGrid.Children.Add(footerGrid);

      _progressBar = new ProgressBar {
        Height = 4,
        IsIndeterminate = true,
        Visibility = Visibility.Collapsed,
        Foreground = new SolidColorBrush(Color.FromRgb(99, 102, 241)),
        Margin = new Thickness(8, 4, 8, 8)
      };
      Grid.SetRow(_progressBar, 0);
      footerGrid.Children.Add(_progressBar);

      var buttonRow = new Grid();
      buttonRow.ColumnDefinitions.Add(new ColumnDefinition());
      buttonRow.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
      buttonRow.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
      buttonRow.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
      buttonRow.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
      Grid.SetRow(buttonRow, 1);
      footerGrid.Children.Add(buttonRow);

      var buttonHint = new TextBlock {
        Text = "",
        Foreground = new SolidColorBrush(Color.FromRgb(195, 209, 232)),
        FontSize = 12,
        TextWrapping = TextWrapping.Wrap,
        Margin = new Thickness(0, 0, 8, 0),
        VerticalAlignment = VerticalAlignment.Center
      };
      buttonRow.Children.Add(buttonHint);

      _continueButton = new Button {
        Content = "Next",
        Height = 40,
        MinWidth = 136,
        Margin = new Thickness(10, 0, 0, 0),
        Padding = new Thickness(16, 0, 16, 0),
        FontWeight = FontWeights.SemiBold,
        Background = new SolidColorBrush(Color.FromRgb(99, 102, 241)),
        Foreground = new SolidColorBrush(Color.FromRgb(245, 249, 255)),
        BorderBrush = new SolidColorBrush(Color.FromRgb(165, 180, 252)),
        BorderThickness = new Thickness(1.5),
        IsDefault = true,
        ToolTip = "Run the selected action"
      };
      _continueButton.MouseEnter += ContinueButtonMouseEnter;
      _continueButton.MouseLeave += ContinueButtonMouseLeave;
      _continueButton.Click += ContinueClicked;
      ApplyFlatButtonTemplate(_continueButton, 8);
      Grid.SetColumn(_continueButton, 1);
      buttonRow.Children.Add(_continueButton);

      _uninstallButton = new Button {
        Content = "Uninstall Vibepollo",
        Height = 40,
        MinWidth = 152,
        Margin = new Thickness(10, 0, 0, 0),
        Padding = new Thickness(16, 0, 16, 0),
        FontWeight = FontWeights.SemiBold,
        Background = new SolidColorBrush(Color.FromRgb(225, 29, 72)),
        Foreground = new SolidColorBrush(Color.FromRgb(245, 249, 255)),
        BorderBrush = new SolidColorBrush(Color.FromRgb(251, 113, 133)),
        BorderThickness = new Thickness(1.5),
        Visibility = Visibility.Visible
      };
      _uninstallButton.MouseEnter += UninstallButtonMouseEnter;
      _uninstallButton.MouseLeave += UninstallButtonMouseLeave;
      _uninstallButton.Click += UninstallNowClicked;
      ApplyFlatButtonTemplate(_uninstallButton, 8);
      Grid.SetColumn(_uninstallButton, 2);
      buttonRow.Children.Add(_uninstallButton);

      _licenseButton = new Button {
        Content = "_License",
        Height = 40,
        MinWidth = 102,
        Margin = new Thickness(10, 0, 0, 0),
        Padding = new Thickness(16, 0, 16, 0),
        FontWeight = FontWeights.SemiBold,
        Background = new SolidColorBrush(Color.FromRgb(16, 24, 42)),
        Foreground = new SolidColorBrush(Color.FromRgb(232, 239, 253)),
        BorderBrush = new SolidColorBrush(Color.FromRgb(82, 96, 141)),
        ToolTip = "View software license"
      };
      _licenseButton.Click += LicenseClicked;
      ApplyFlatButtonTemplate(_licenseButton, 8);
      Grid.SetColumn(_licenseButton, 3);
      buttonRow.Children.Add(_licenseButton);

      _closeButton = new Button {
        Content = "Cl_ose",
        Height = 40,
        MinWidth = 102,
        Margin = new Thickness(10, 0, 0, 0),
        Padding = new Thickness(16, 0, 16, 0),
        FontWeight = FontWeights.SemiBold,
        Background = new SolidColorBrush(Color.FromRgb(16, 24, 42)),
        Foreground = new SolidColorBrush(Color.FromRgb(232, 239, 253)),
        BorderBrush = new SolidColorBrush(Color.FromRgb(82, 96, 141)),
        IsCancel = true,
        ToolTip = "Close this installer window"
      };
      _closeButton.Click += (sender, eventArgs) => Close();
      ApplyFlatButtonTemplate(_closeButton, 8);
      Grid.SetColumn(_closeButton, 4);
      buttonRow.Children.Add(_closeButton);

      _continueButton.Content = BuildFlavor.IsUninstallOnly ? "Uninstall Vibepollo" : BuildInstallButtonLabel();
      if (_uninstallUiRequested && _installedProduct == null) {
        SetStatus(
          "Vibepollo is not installed.",
          BuildFlavor.IsUninstallOnly
            ? "No uninstall action is required."
            : "Uninstall is unavailable. Choose Install Vibepollo to continue.",
          _statusNormalBrush);
      } else {
        SetStatus("Ready.", string.Empty, _statusNormalBrush);
      }
      UpdateActionUiState();
      Loaded += InstallerWindowLoaded;
    }

    private static Brush CreateBackgroundBrush() {
      var brush = new LinearGradientBrush();
      brush.StartPoint = new Point(0, 0);
      brush.EndPoint = new Point(1, 1);
      brush.GradientStops.Add(new GradientStop(Color.FromRgb(6, 10, 24), 0.0));
      brush.GradientStops.Add(new GradientStop(Color.FromRgb(14, 20, 36), 0.42));
      brush.GradientStops.Add(new GradientStop(Color.FromRgb(12, 18, 34), 1.0));
      return brush;
    }

    private static void ApplyFlatButtonTemplate(Button button, double cornerRadius) {
      button.OverridesDefaultStyle = true;
      button.Template = CreateFlatButtonTemplate(cornerRadius);
      button.FocusVisualStyle = null;
      button.Cursor = Cursors.Hand;
    }

    private static ControlTemplate CreateFlatButtonTemplate(double cornerRadius) {
      var border = new FrameworkElementFactory(typeof(Border));
      border.SetValue(Border.CornerRadiusProperty, new CornerRadius(cornerRadius));
      border.SetValue(Border.SnapsToDevicePixelsProperty, true);
      border.SetValue(Border.BackgroundProperty, new TemplateBindingExtension(Control.BackgroundProperty));
      border.SetValue(Border.BorderBrushProperty, new TemplateBindingExtension(Control.BorderBrushProperty));
      border.SetValue(Border.BorderThicknessProperty, new TemplateBindingExtension(Control.BorderThicknessProperty));

      var content = new FrameworkElementFactory(typeof(ContentPresenter));
      content.SetValue(ContentPresenter.RecognizesAccessKeyProperty, true);
      content.SetValue(ContentPresenter.SnapsToDevicePixelsProperty, true);
      content.SetValue(ContentPresenter.ContentProperty, new TemplateBindingExtension(ContentControl.ContentProperty));
      content.SetValue(ContentPresenter.ContentTemplateProperty, new TemplateBindingExtension(ContentControl.ContentTemplateProperty));
      content.SetValue(ContentPresenter.HorizontalAlignmentProperty, new TemplateBindingExtension(Control.HorizontalContentAlignmentProperty));
      content.SetValue(ContentPresenter.VerticalAlignmentProperty, new TemplateBindingExtension(Control.VerticalContentAlignmentProperty));
      content.SetValue(ContentPresenter.MarginProperty, new TemplateBindingExtension(Control.PaddingProperty));
      content.SetValue(TextElement.ForegroundProperty, new TemplateBindingExtension(Control.ForegroundProperty));
      border.AppendChild(content);

      var template = new ControlTemplate(typeof(Button));
      template.VisualTree = border;

      var pressedTrigger = new Trigger {
        Property = Button.IsPressedProperty,
        Value = true
      };
      pressedTrigger.Setters.Add(new Setter(UIElement.OpacityProperty, 0.92));
      template.Triggers.Add(pressedTrigger);

      var disabledTrigger = new Trigger {
        Property = UIElement.IsEnabledProperty,
        Value = false
      };
      disabledTrigger.Setters.Add(new Setter(UIElement.OpacityProperty, 0.58));
      template.Triggers.Add(disabledTrigger);

      return template;
    }

    private void TitleBarMouseLeftButtonDown(object sender, MouseButtonEventArgs e) {
      if (e.ChangedButton == MouseButton.Left) {
        DragMove();
      }
    }

    private void MinimizeClicked(object sender, RoutedEventArgs e) {
      WindowState = WindowState.Minimized;
    }

    private void InstallerWindowLoaded(object sender, RoutedEventArgs e) {
      BringWindowToFront();
      FocusDefaultActionControl();
      Dispatcher.BeginInvoke(new Action(() => {
        BringWindowToFront();
        FocusDefaultActionControl();
      }), DispatcherPriority.ContextIdle);
    }

    private void FocusDefaultActionControl() {
      if (_installSection.Visibility == Visibility.Visible) {
        _installPathTextBox.Focus();
        _installPathTextBox.SelectAll();
      } else if (BuildFlavor.IsUninstallOnly && _uninstallButton.Visibility == Visibility.Visible) {
        _uninstallButton.Focus();
      } else {
        _continueButton.Focus();
      }
    }

    private void BringWindowToFront() {
      if (WindowState == WindowState.Minimized) {
        WindowState = WindowState.Normal;
      }

      Show();
      Activate();

      var handle = new WindowInteropHelper(this).Handle;
      if (handle == IntPtr.Zero) {
        return;
      }

      ShowWindow(handle, SW_RESTORE);
      SetWindowPos(handle, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
      SetWindowPos(handle, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
      BringWindowToTop(handle);
      SetForegroundWindow(handle);
    }

    protected override void OnClosed(EventArgs e) {
      StopOverlayAutoClose();
      base.OnClosed(e);
    }

    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool SetForegroundWindow(IntPtr hWnd);

    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool BringWindowToTop(IntPtr hWnd);

    [DllImport("user32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool ShowWindow(IntPtr hWnd, int nCmdShow);

    [DllImport("user32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool SetWindowPos(
      IntPtr hWnd,
      IntPtr hWndInsertAfter,
      int X,
      int Y,
      int cx,
      int cy,
      uint uFlags);

    private void TitleMinimizeMouseEnter(object sender, MouseEventArgs e) {
      var button = sender as Button;
      if (button == null) {
        return;
      }
      button.Background = new SolidColorBrush(Color.FromRgb(58, 76, 122));
    }

    private void TitleCloseMouseEnter(object sender, MouseEventArgs e) {
      _titleCloseButton.Background = new SolidColorBrush(Color.FromRgb(224, 46, 90));
      _titleCloseButton.Foreground = Brushes.White;
    }

    private void TitleButtonMouseLeave(object sender, MouseEventArgs e) {
      var button = sender as Button;
      if (button == null) {
        return;
      }
      button.Background = Brushes.Transparent;
      button.Foreground = new SolidColorBrush(Color.FromRgb(224, 236, 255));
    }

    private void ContinueButtonMouseEnter(object sender, MouseEventArgs e) {
      _continueButton.Background = new SolidColorBrush(Color.FromRgb(129, 140, 248));
      _continueButton.BorderBrush = new SolidColorBrush(Color.FromRgb(199, 210, 254));
    }

    private void ContinueButtonMouseLeave(object sender, MouseEventArgs e) {
      _continueButton.Background = new SolidColorBrush(Color.FromRgb(99, 102, 241));
      _continueButton.BorderBrush = new SolidColorBrush(Color.FromRgb(165, 180, 252));
    }

    private void UninstallButtonMouseEnter(object sender, MouseEventArgs e) {
      _uninstallButton.Background = new SolidColorBrush(Color.FromRgb(244, 63, 94));
      _uninstallButton.BorderBrush = new SolidColorBrush(Color.FromRgb(253, 164, 175));
    }

    private void UninstallButtonMouseLeave(object sender, MouseEventArgs e) {
      _uninstallButton.Background = new SolidColorBrush(Color.FromRgb(225, 29, 72));
      _uninstallButton.BorderBrush = new SolidColorBrush(Color.FromRgb(251, 113, 133));
    }

    private void BrowseClicked(object sender, RoutedEventArgs e) {
      var currentPath = _installPathTextBox.Text;
      if (string.IsNullOrWhiteSpace(currentPath)) {
        currentPath = InstallerRunner.DefaultInstallDirectory;
      }

      var selectedPath = ModernFolderPicker.TryPickFolder(this, "Select the Vibepollo install folder", currentPath);
      if (!string.IsNullOrWhiteSpace(selectedPath)) {
        _installPathTextBox.Text = selectedPath;
      }
    }

    private async void ContinueClicked(object sender, RoutedEventArgs e) {
      if (BuildFlavor.IsUninstallOnly) {
        await RunUninstallFlow();
        return;
      }
      await RunInstallFlow();
    }

    private async void UninstallNowClicked(object sender, RoutedEventArgs e) {
      if (_installedProduct == null) {
        SetStatus("Uninstall not started.", "No Vibepollo installation was found on this PC.", _statusNormalBrush);
        await ShowOverlayInfoAsync("Nothing to uninstall", "Vibepollo is not currently installed on this PC.");
        return;
      }

      await RunUninstallFlow();
    }

    private async void LicenseClicked(object sender, RoutedEventArgs e) {
      await ShowLicenseDialogAsync();
    }

    private async Task RunInstallFlow() {
      string selectedPath = null;
      string installPathFailureDetail = null;
      try {
        selectedPath = NormalizeInstallPath(_installPathTextBox.Text);
      } catch (Exception ex) {
        installPathFailureDetail = BuildFailureDetail(ex.Message);
      }
      if (!string.IsNullOrWhiteSpace(installPathFailureDetail)) {
        SetStatus("Install not started.", installPathFailureDetail, _statusErrorBrush);
        await ShowOverlayInfoAsync("Installer error", installPathFailureDetail);
        return;
      }
      _installPathTextBox.Text = selectedPath;

      var vibeshineProduct = InstallerRunner.GetInstalledVibeshineProduct();
      if (vibeshineProduct != null) {
        var proceed = await ShowOverlayConfirmAsync(
          "Sunshine ecosystem detected",
          BuildVibeshineInstallWarning(vibeshineProduct),
          "Continue with Vibepollo",
          "Cancel",
          false);
        if (!proceed) {
          SetStatus("Install canceled.", "No changes were made.", _statusNormalBrush);
          return;
        }
      }

      var apolloProduct = InstallerRunner.GetInstalledApolloProduct();
      if (apolloProduct != null) {
        var proceed = await ShowOverlayConfirmAsync(
          "Apollo detected",
          BuildApolloInstallWarning(apolloProduct),
          "Uninstall Apollo",
          "Cancel",
          false);
        if (!proceed) {
          SetStatus("Install canceled.", "No changes were made.", _statusNormalBrush);
          return;
        }
      }

      if (_legacyApolloRegistration != null) {
        var proceed = await ShowOverlayConfirmAsync(
          "Legacy Apollo detected",
          BuildLegacyApolloMigrationWarning(),
          "Uninstall Apollo",
          "Cancel",
          false);
        if (!proceed) {
          SetStatus("Install canceled.", "No changes were made.", _statusNormalBrush);
          return;
        }
      }

      if (_legacySunshineProduct != null || _legacySunshineRegistration != null) {
        var proceedWithRemoval = await ShowOverlayConfirmAsync(
          "Sunshine detected",
          BuildLegacySunshineMigrationWarning(),
          "Uninstall Sunshine",
          "Cancel",
          false);
        if (!proceedWithRemoval) {
          SetStatus("Install canceled.", "No changes were made.", _statusNormalBrush);
          return;
        }
      }

      // Warn about low disk space but allow the user to proceed
      var spaceWarning = CheckDiskSpace(selectedPath);
      if (spaceWarning != null) {
        var proceed = await ShowOverlayConfirmAsync("Low disk space", spaceWarning + "\n\nContinue anyway?", "Continue", "Cancel", false);
        if (!proceed) {
          SetStatus("Install not started.", "Choose a drive with more free space.", _statusNormalBrush);
          return;
        }
      }

      await RunOperationAsync(async () => {
        var installVirtualDisplayDriver = _installVirtualDisplayCheckBox.IsChecked == true;
        return await Task.Run(() => InstallerRunner.RunInteractiveInstall(
          _arguments,
          selectedPath,
          installVirtualDisplayDriver,
          false));
      }, "Install", "Installing or updating Vibepollo...", "Vibepollo installation completed.");
    }

    private async Task RunUninstallFlow() {
      if (_installedProduct == null) {
        SetStatus("Uninstall not started.", "No Vibepollo installation was found on this PC.", _statusNormalBrush);
        await ShowOverlayInfoAsync("Nothing to uninstall", "Vibepollo is not currently installed on this PC.");
        return;
      }

      var uninstallOptions = await ShowOverlayUninstallOptionsAsync();
      if (uninstallOptions == null) {
        SetStatus("Uninstall canceled.", "No changes were made.", _statusNormalBrush);
        return;
      }

      await RunOperationAsync(
        () => Task.Run(() => InstallerRunner.RunInteractiveUninstall(
          _arguments,
          uninstallOptions.Value.DeleteInstallDirectory,
          uninstallOptions.Value.RemoveVirtualDisplayDriver)),
        "Uninstall",
        "Removing Vibepollo...",
        "Vibepollo uninstall completed.");
    }

    private async Task RunOperationAsync(Func<Task<InstallerResult>> actionFactory, string actionLabel, string inProgressText, string successText) {
      SetBusyState(true);
      SetStatus(inProgressText, "This can take a minute. Admin approval is requested only after the operation starts.", _statusBusyBrush);
      string exceptionFailureDetail = null;
      try {
        var result = await actionFactory();
        if (result.Succeeded) {
          ProcessExitCode = 0;
          if (result.Operation == InstallerOperation.Install && result.PartiallySucceeded) {
            var warningDetail = BuildComponentFailureDetail(result.ComponentFailures);
            if (!string.IsNullOrWhiteSpace(result.UserDetail)) {
              warningDetail += "\n" + result.UserDetail;
            }
            SetStatus("Vibepollo installation completed with warnings.", warningDetail, _statusWarningBrush);
            await ShowInstallPartialSuccessDialogAsync(result);
            Close();
            return;
          }
          var detail = result.ExitCode == 3010
            ? "The operation completed and Windows restart is required."
            : "All selected changes were applied successfully.";
          if (!string.IsNullOrWhiteSpace(result.UserDetail)) {
            detail += "\n" + result.UserDetail;
          }
          SetStatus(successText, detail, _statusSuccessBrush);
          await ShowOverlayInfoAsync("Complete", _statusText.Text);
          Close();
          return;
        }

        if (result.ExitCode == 1223) {
          ProcessExitCode = result.ExitCode;
          SetStatus(
            actionLabel + " cancelled.",
            "No changes were made because the elevation prompt was cancelled.",
            _statusNormalBrush);
          return;
        }

        if (result.Operation == InstallerOperation.Uninstall && result.ExitCode == 1605) {
          ProcessExitCode = 0;
          SetStatus(
            "Vibepollo is not installed.",
            "Nothing needed to be removed.",
            _statusNormalBrush);
          await ShowOverlayInfoAsync("Nothing to uninstall", "Vibepollo is not currently installed on this PC.");
          return;
        }

        ProcessExitCode = result.ExitCode;
        var failureDetail = BuildFailureDetail(result.Message);
        SetStatus(actionLabel + " failed.", failureDetail, _statusErrorBrush);
        if (result.Operation == InstallerOperation.Install) {
          await ShowInstallFailureSupportDialogAsync(failureDetail, result);
        } else {
          await ShowOverlayInfoAsync("Installer error", failureDetail);
        }
      } catch (Exception ex) {
        ProcessExitCode = 1;
        exceptionFailureDetail = BuildFailureDetail(ex.Message);
        SetStatus(actionLabel + " failed.", exceptionFailureDetail, _statusErrorBrush);
      } finally {
        SetBusyState(false);
      }

      if (!string.IsNullOrWhiteSpace(exceptionFailureDetail)) {
        var failedOperation = string.Equals(actionLabel, "Install", StringComparison.OrdinalIgnoreCase)
          ? InstallerOperation.Install
          : InstallerOperation.Uninstall;
        if (failedOperation == InstallerOperation.Install) {
          await ShowInstallFailureSupportDialogAsync(exceptionFailureDetail, new InstallerResult {
            Operation = InstallerOperation.Install,
            ExitCode = 1,
            Message = exceptionFailureDetail
          });
        } else {
          await ShowOverlayInfoAsync("Installer error", exceptionFailureDetail);
        }
      }
    }

    private static string BuildComponentFailureDetail(IReadOnlyList<string> componentFailures) {
      if (componentFailures == null || componentFailures.Count == 0) {
        return "The operation completed with warnings.";
      }

      var lines = new List<string> {
        "The operation completed, but some components failed:"
      };
      foreach (var failure in componentFailures.Where(item => !string.IsNullOrWhiteSpace(item))) {
        lines.Add("- " + failure.Trim());
      }
      return string.Join("\n", lines);
    }

    private static string NormalizeInstallPath(string installPath) {
      var trimmedPath = (installPath ?? string.Empty).Trim();
      if (trimmedPath.Length == 0) {
        throw new InvalidOperationException("Choose an install folder before clicking Install or Update.");
      }

      string fullPath;
      try {
        fullPath = Path.GetFullPath(trimmedPath);
      } catch (Exception ex) {
        if (ex is ArgumentException || ex is NotSupportedException || ex is PathTooLongException) {
          throw new InvalidOperationException("The install folder path is invalid. Choose a different folder and try again.");
        }
        throw;
      }

      // Block UNC / network paths — Windows services cannot reliably run from network locations
      if (fullPath.StartsWith(@"\\", StringComparison.Ordinal)) {
        throw new InvalidOperationException("Network paths (UNC) are not supported. Vibepollo runs as a Windows service and must be installed on a local drive.");
      }

      // Verify the drive exists
      var root = Path.GetPathRoot(fullPath);
      if (!string.IsNullOrEmpty(root) && !Directory.Exists(root)) {
        throw new InvalidOperationException("The drive " + root.TrimEnd('\\') + " does not exist. Choose an install folder on an available local drive.");
      }

      return fullPath;
    }

    /// <summary>
    /// Checks available disk space and returns a warning message, or null if space is sufficient.
    /// </summary>
    private static string CheckDiskSpace(string installPath) {
      const long MinimumBytesRequired = 500L * 1024 * 1024; // 500 MB
      try {
        var root = Path.GetPathRoot(installPath);
        if (string.IsNullOrEmpty(root)) return null;
        var driveInfo = new DriveInfo(root);
        if (driveInfo.IsReady && driveInfo.AvailableFreeSpace < MinimumBytesRequired) {
          var availableMb = driveInfo.AvailableFreeSpace / (1024 * 1024);
          return "Warning: Drive " + root.TrimEnd('\\') + " has only " + availableMb + " MB free. "
            + "At least 500 MB is recommended. The installation may fail if there is not enough space.";
        }
      } catch {
        // Non-fatal — skip the check if drive info is unavailable
      }
      return null;
    }

    private static string BuildFailureDetail(string message) {
      if (!string.IsNullOrWhiteSpace(message)) {
        return message;
      }
      return "The operation did not complete as expected. Check the MSI log in " + Path.GetTempPath() + " for details, or try running the installer as Administrator.";
    }

    private static string BuildVibeshineInstallWarning(InstallerRunner.InstalledProductInfo vibeshineProduct) {
      var versionSuffix = vibeshineProduct != null && vibeshineProduct.Version != null
        ? " (v" + vibeshineProduct.Version.ToString(3) + ")"
        : string.Empty;

      return "Vibeshine" + versionSuffix + " was detected on this PC.\n\n"
        + "Vibeshine does not carry over Vibeshine settings.\n"
        + "If you intend to stay in the Sunshine ecosystem, Vibeshine is recommended instead.\n\n"
        + "If this is intentional, continue with Vibeshine.\n"
        + "Continuing will uninstall Vibeshine before installation.";
    }

    private static string BuildApolloInstallWarning(InstallerRunner.InstalledProductInfo apolloProduct) {
      var versionSuffix = apolloProduct != null && apolloProduct.Version != null
        ? " (v" + apolloProduct.Version.ToString(3) + ")"
        : string.Empty;

      return "Apollo" + versionSuffix + " was detected on this PC.\n\n"
        + "Vibeshine replaces Apollo and cannot be installed while Apollo is installed.\n"
        + "Continuing will uninstall Apollo before installation.\n\n"
        + "Click Uninstall Apollo to proceed.";
    }

    private string BuildLegacySunshineMigrationWarning() {
      var versionSuffix = string.Empty;
      if (_legacySunshineProduct != null && _legacySunshineProduct.Version != null) {
        versionSuffix = " (v" + _legacySunshineProduct.Version.ToString(3) + ")";
      } else if (!string.IsNullOrWhiteSpace(_legacySunshineRegistration == null ? null : _legacySunshineRegistration.DisplayVersion)) {
        versionSuffix = " (v" + _legacySunshineRegistration.DisplayVersion + ")";
      }

      return "Sunshine" + versionSuffix + " was detected on this PC.\n\n"
        + "Vibeshine replaces Sunshine and will automatically uninstall Sunshine first, then install Vibeshine.\n"
        + "No settings will be lost during this migration.\n\n"
        + "Click Uninstall Sunshine to proceed.";
    }

    private string BuildLegacyApolloMigrationWarning() {
      var versionSuffix = string.Empty;
      if (_legacyApolloRegistration != null && !string.IsNullOrWhiteSpace(_legacyApolloRegistration.DisplayVersion)) {
        versionSuffix = " (v" + _legacyApolloRegistration.DisplayVersion + ")";
      }

      return "Legacy Apollo" + versionSuffix + " was detected on this PC.\n\n"
        + "Vibeshine replaces legacy Apollo and will automatically uninstall it first, then install Vibeshine.\n"
        + "No settings will be carried over.\n\n"
        + "Click Uninstall Apollo to proceed.";
    }

    private enum InstallActionKind {
      Install,
      Upgrade,
      Downgrade,
      Reinstall
    }

    private InstallActionKind GetInstallActionKind() {
      if (_installedProduct == null) {
        return InstallActionKind.Install;
      }

      var targetVersion = _payloadMsiInfo == null ? _bundleVersion : _payloadMsiInfo.Version;
      if (_installedProduct.Version != null && targetVersion != null && _installedProduct.Version > targetVersion) {
        return InstallActionKind.Downgrade;
      }

      if (WillPayloadMsiUpgradeInstalledProduct()) {
        return InstallActionKind.Upgrade;
      }

      return InstallActionKind.Reinstall;
    }

    private bool WillPayloadMsiUpgradeInstalledProduct() {
      if (_installedProduct == null || _payloadMsiInfo == null) {
        return false;
      }
      if (string.IsNullOrWhiteSpace(_installedProduct.ProductCode) || string.IsNullOrWhiteSpace(_payloadMsiInfo.ProductCode)) {
        return false;
      }
      if (_payloadMsiInfo.RelatedProductCodes == null || _payloadMsiInfo.RelatedProductCodes.Count == 0) {
        return false;
      }

      var hasRelatedInstalledProduct = _payloadMsiInfo.RelatedProductCodes.Any(code =>
        string.Equals(code, _installedProduct.ProductCode, StringComparison.OrdinalIgnoreCase));
      if (!hasRelatedInstalledProduct) {
        return false;
      }

      return !string.Equals(_installedProduct.ProductCode, _payloadMsiInfo.ProductCode, StringComparison.OrdinalIgnoreCase);
    }

    private string GetTargetVersionText() {
      if (_payloadMsiInfo != null && !string.IsNullOrWhiteSpace(_payloadMsiInfo.VersionText)) {
        return _payloadMsiInfo.VersionText;
      }
      return _bundleVersion.ToString(3);
    }

    private string BuildInstallButtonLabel() {
      switch (GetInstallActionKind()) {
        case InstallActionKind.Install:
          return "Install Vibepollo";
        case InstallActionKind.Upgrade:
          return "Upgrade Vibepollo";
        case InstallActionKind.Downgrade:
          return "Downgrade Vibepollo";
        default:
          return "Reinstall Vibepollo";
      }
    }

    private async Task ShowLicenseDialogAsync() {
      var maxTextHeight = ActualHeight - 320;
      if (maxTextHeight < 140) {
        maxTextHeight = 140;
      }
      if (maxTextHeight > 230) {
        maxTextHeight = 230;
      }

      await ShowOverlayAsync(
        "License",
        "Vibepollo software license terms:",
        "Close",
        string.Empty,
        new SolidColorBrush(Color.FromRgb(99, 102, 241)),
        new SolidColorBrush(Color.FromRgb(165, 180, 252)),
        false,
        content => {
          var licenseTextBox = new TextBox {
            Text = _licenseText,
            IsReadOnly = true,
            AcceptsReturn = true,
            TextWrapping = TextWrapping.Wrap,
            VerticalScrollBarVisibility = ScrollBarVisibility.Auto,
            HorizontalScrollBarVisibility = ScrollBarVisibility.Disabled,
            MinHeight = maxTextHeight,
            MaxHeight = maxTextHeight,
            Background = new SolidColorBrush(Color.FromRgb(8, 14, 28)),
            Foreground = new SolidColorBrush(Color.FromRgb(226, 235, 250)),
            BorderBrush = new SolidColorBrush(Color.FromRgb(82, 96, 141)),
            Margin = new Thickness(0, 0, 0, 4),
            Padding = new Thickness(12)
          };
          content.Children.Add(licenseTextBox);
        },
        0);
    }

    private static string LoadEmbeddedLicenseText() {
      const string fallbackText = "License text is unavailable in this installer build.";
      try {
        var assembly = Assembly.GetExecutingAssembly();
        using (var stream = assembly.GetManifestResourceStream("License.txt")) {
          if (stream == null) {
            return fallbackText;
          }

          using (var reader = new StreamReader(stream)) {
            var content = reader.ReadToEnd();
            if (string.IsNullOrWhiteSpace(content)) {
              return fallbackText;
            }
            return content;
          }
        }
      } catch {
        return fallbackText;
      }
    }

    private void SetBusyState(bool busy) {
      _isBusy = busy;
      UpdateActionUiState();
      _continueButton.IsEnabled = !busy;
      _uninstallButton.IsEnabled = !busy && _installedProduct != null;
      _licenseButton.IsEnabled = !busy;
      _closeButton.IsEnabled = !busy;
      _titleCloseButton.IsEnabled = !busy;
      _titleCloseButton.Opacity = busy ? 0.6 : 1.0;
      _progressBar.Visibility = busy ? Visibility.Visible : Visibility.Collapsed;
    }

    private void UpdateActionUiState() {
      if (BuildFlavor.IsUninstallOnly) {
        var allowUninstall = !_isBusy && _installedProduct != null;
        _installPathTextBox.IsEnabled = false;
        _installVirtualDisplayCheckBox.IsEnabled = false;
        _browseButton.IsEnabled = false;
        _installSection.Visibility = Visibility.Collapsed;
        _continueButton.Visibility = Visibility.Collapsed;
        _uninstallButton.Visibility = Visibility.Visible;
        _uninstallButton.IsEnabled = allowUninstall;
        return;
      }

      var allowInstallInputs = !_isBusy;
      _installPathTextBox.IsEnabled = allowInstallInputs;
      _installVirtualDisplayCheckBox.IsEnabled = allowInstallInputs;
      _browseButton.IsEnabled = allowInstallInputs;
      var hasInstalledProduct = _installedProduct != null;
      _installSection.Visibility = hasInstalledProduct ? Visibility.Collapsed : Visibility.Visible;
      _uninstallButton.Visibility = hasInstalledProduct ? Visibility.Visible : Visibility.Collapsed;
      _continueButton.Visibility = Visibility.Visible;
      _continueButton.Content = BuildInstallButtonLabel();
    }

    private void ResolveOverlay(string result) {
      StopOverlayAutoClose();
      var tcs = _overlayTcs;
      if (tcs == null || tcs.Task.IsCompleted) {
        return;
      }
      _overlayGrid.Visibility = Visibility.Collapsed;
      _overlayTcs = null;
      tcs.TrySetResult(result);
    }

    private Task<string> ShowOverlayAsync(
      string title,
      string message,
      string primaryText,
      string secondaryText,
      Brush primaryBackground,
      Brush primaryBorder,
      bool showSecondary,
      Action<StackPanel> buildContent,
      int autoCloseSeconds) {
      if (_overlayTcs != null && !_overlayTcs.Task.IsCompleted) {
        _overlayTcs.TrySetResult("secondary");
      }
      StopOverlayAutoClose();

      _overlayTitleText.Text = title ?? string.Empty;
      _overlayMessageText.Text = message ?? string.Empty;
      _overlayAccentBar.Background = primaryBackground ?? new SolidColorBrush(Color.FromRgb(99, 102, 241));
      _overlayContentHost.Children.Clear();
      if (buildContent != null) {
        buildContent(_overlayContentHost);
      }

      _overlayPrimaryButton.Content = primaryText ?? "OK";
      _overlayPrimaryButton.Background = primaryBackground ?? new SolidColorBrush(Color.FromRgb(99, 102, 241));
      _overlayPrimaryButton.BorderBrush = primaryBorder ?? new SolidColorBrush(Color.FromRgb(165, 180, 252));

      _overlaySecondaryButton.Content = secondaryText ?? "Cancel";
      _overlaySecondaryButton.Visibility = showSecondary ? Visibility.Visible : Visibility.Collapsed;
      if (autoCloseSeconds > 0 && !showSecondary) {
        _overlayHintText.Visibility = Visibility.Visible;
        _overlayAutoCloseProgressBar.Visibility = Visibility.Visible;
        StartOverlayAutoClose(autoCloseSeconds);
      } else {
        _overlayHintText.Text = string.Empty;
        _overlayHintText.Visibility = Visibility.Collapsed;
        _overlayAutoCloseProgressBar.Visibility = Visibility.Collapsed;
        _overlayAutoCloseProgressBar.Value = 0;
      }

      _overlayGrid.Visibility = Visibility.Visible;
      _overlayPrimaryButton.Focus();

      _overlayTcs = new TaskCompletionSource<string>();
      return _overlayTcs.Task;
    }

    private void StartOverlayAutoClose(int seconds) {
      _overlayAutoCloseSecondsRemaining = seconds <= 0 ? 0 : seconds;
      if (_overlayAutoCloseSecondsRemaining <= 0) {
        return;
      }

      _overlayAutoCloseDeadlineUtc = DateTime.UtcNow.AddSeconds(_overlayAutoCloseSecondsRemaining);
      _overlayAutoCloseTotalSeconds = _overlayAutoCloseSecondsRemaining;
      _overlayAutoCloseProgressBar.Minimum = 0;
      _overlayAutoCloseProgressBar.Maximum = _overlayAutoCloseTotalSeconds;
      _overlayAutoCloseProgressBar.Value = 0;
      UpdateOverlayAutoCloseCountdownUi();
      _overlayAutoCloseTimer = new DispatcherTimer {
        Interval = TimeSpan.FromMilliseconds(100)
      };
      _overlayAutoCloseTimer.Tick += OverlayAutoCloseTimerTick;
      _overlayAutoCloseTimer.Start();
    }

    private void StopOverlayAutoClose() {
      if (_overlayAutoCloseTimer == null) {
        return;
      }
      _overlayAutoCloseTimer.Stop();
      _overlayAutoCloseTimer.Tick -= OverlayAutoCloseTimerTick;
      _overlayAutoCloseTimer = null;
      _overlayAutoCloseSecondsRemaining = 0;
      _overlayAutoCloseTotalSeconds = 0;
      _overlayAutoCloseProgressBar.Value = 0;
    }

    private void OverlayAutoCloseTimerTick(object sender, EventArgs e) {
      if (UpdateOverlayAutoCloseCountdownUi()) {
        ResolveOverlay("primary");
      }
    }

    private bool UpdateOverlayAutoCloseCountdownUi() {
      var secondsRemaining = (_overlayAutoCloseDeadlineUtc - DateTime.UtcNow).TotalSeconds;
      if (secondsRemaining < 0) {
        secondsRemaining = 0;
      }

      var secondsElapsed = _overlayAutoCloseTotalSeconds - secondsRemaining;
      if (secondsElapsed < 0) {
        secondsElapsed = 0;
      }
      if (secondsElapsed > _overlayAutoCloseProgressBar.Maximum) {
        secondsElapsed = _overlayAutoCloseProgressBar.Maximum;
      }
      _overlayAutoCloseProgressBar.Value = secondsElapsed;
      var displaySeconds = (int)Math.Ceiling(secondsRemaining);
      if (displaySeconds <= 0) {
        _overlayHintText.Text = "Closing…";
        return true;
      }

      _overlayHintText.Text = "This message closes automatically in " + displaySeconds + " seconds.";
      return false;
    }

    private async Task<bool> ShowOverlayConfirmAsync(string title, string message, string confirmText, string cancelText, bool destructive) {
      var primaryBg = destructive
        ? (Brush)new SolidColorBrush(Color.FromRgb(225, 29, 72))
        : new SolidColorBrush(Color.FromRgb(99, 102, 241));
      var primaryBorder = destructive
        ? (Brush)new SolidColorBrush(Color.FromRgb(251, 113, 133))
        : new SolidColorBrush(Color.FromRgb(165, 180, 252));
      var result = await ShowOverlayAsync(title, message, confirmText, cancelText, primaryBg, primaryBorder, true, null, 0);
      return string.Equals(result, "primary", StringComparison.OrdinalIgnoreCase);
    }

    private async Task ShowOverlayInfoAsync(string title, string message) {
      await ShowOverlayAsync(
        title,
        message,
        "OK",
        string.Empty,
        new SolidColorBrush(Color.FromRgb(99, 102, 241)),
        new SolidColorBrush(Color.FromRgb(165, 180, 252)),
        false,
        null,
        5);
    }

    private struct UninstallOptions {
      public bool RemoveVirtualDisplayDriver;
      public bool DeleteInstallDirectory;
    }

    private async Task<UninstallOptions?> ShowOverlayUninstallOptionsAsync() {
      var removeDriverCheckBox = new CheckBox {
        Content = "Remove virtual display driver (SudoVDA)",
        FontSize = 13,
        Foreground = new SolidColorBrush(Color.FromRgb(226, 235, 250)),
        Margin = new Thickness(0, 0, 0, 8),
        IsChecked = false
      };
      var deleteFolderCheckBox = new CheckBox {
        Content = "Factory reset (deletes all settings back to default)",
        FontSize = 13,
        Foreground = new SolidColorBrush(Color.FromRgb(226, 235, 250)),
        Margin = new Thickness(0, 0, 0, 0),
        IsChecked = false
      };

      var message = "Choose what to remove during uninstall.\n\n"
        + "Uninstall always removes the Vibepollo service, firewall rules, and program files.";

      var result = await ShowOverlayAsync(
        "Uninstall Vibepollo",
        message,
        "Uninstall",
        "Cancel",
        new SolidColorBrush(Color.FromRgb(225, 29, 72)),
        new SolidColorBrush(Color.FromRgb(251, 113, 133)),
        true,
        content => {
          content.Children.Add(removeDriverCheckBox);
          content.Children.Add(deleteFolderCheckBox);
        },
        0);

      if (!string.Equals(result, "primary", StringComparison.OrdinalIgnoreCase)) {
        return null;
      }

      return new UninstallOptions {
        RemoveVirtualDisplayDriver = removeDriverCheckBox.IsChecked == true,
        DeleteInstallDirectory = deleteFolderCheckBox.IsChecked == true
      };
    }

    private async Task ShowInstallFailureSupportDialogAsync(string failureDetail, InstallerResult installResult) {
      var decision = await ShowOverlayAsync(
        "Install failed",
        failureDetail + "\n\nSave logs now, then report this issue on GitHub or Discord.",
        "Save logs",
        "Not now",
        new SolidColorBrush(Color.FromRgb(99, 102, 241)),
        new SolidColorBrush(Color.FromRgb(165, 180, 252)),
        true,
        content => {
          content.Children.Add(BuildSupportLinksTextBlock());
        },
        0);

      if (!string.Equals(decision, "primary", StringComparison.OrdinalIgnoreCase)) {
        return;
      }

      await SaveInstallFailureSupportBundleAsync(failureDetail, installResult);
    }

    private async Task ShowInstallPartialSuccessDialogAsync(InstallerResult installResult) {
      var warningDetail = BuildComponentFailureDetail(installResult == null ? null : installResult.ComponentFailures);
      var decision = await ShowOverlayAsync(
        "Install completed with warnings",
        warningDetail + "\n\nSave logs now, then report this issue on GitHub or Discord.",
        "Save logs",
        "Not now",
        new SolidColorBrush(Color.FromRgb(99, 102, 241)),
        new SolidColorBrush(Color.FromRgb(165, 180, 252)),
        true,
        content => {
          content.Children.Add(BuildSupportLinksTextBlock());
        },
        0);

      if (!string.Equals(decision, "primary", StringComparison.OrdinalIgnoreCase)) {
        return;
      }

      await SaveInstallWarningSupportBundleAsync(warningDetail, installResult);
    }

    private async Task SaveInstallFailureSupportBundleAsync(string failureDetail, InstallerResult installResult) {
      var timestamp = DateTime.Now.ToString("yyyyMMdd_HHmmss");
      var saveDialog = new SaveFileDialog {
        Title = "Save support logs",
        Filter = "Log report (*.txt)|*.txt",
        DefaultExt = ".txt",
        AddExtension = true,
        OverwritePrompt = true,
        FileName = "vibeshine-install-logs-" + timestamp + ".txt",
        InitialDirectory = Environment.GetFolderPath(Environment.SpecialFolder.DesktopDirectory)
      };

      var selected = saveDialog.ShowDialog(this);
      if (selected != true || string.IsNullOrWhiteSpace(saveDialog.FileName)) {
        return;
      }

      string error = null;
      var outputPath = saveDialog.FileName;
      try {
        await Task.Run(() => WriteInstallFailureSupportReport(outputPath, failureDetail, installResult));
      } catch (Exception ex) {
        error = ex.Message;
      }

      if (!string.IsNullOrWhiteSpace(error)) {
        SetStatus("Could not save support logs.", error, _statusErrorBrush);
        await ShowOverlayInfoAsync("Could not save logs", error);
        return;
      }

      var nextStep = "Attach this file on GitHub: https://github.com/Nonary/Vibepollo/issues\n"
        + "Or Discord (#vibeshine): https://discord.com/invite/CGg5JxN";
      SetStatus("Support logs saved.", outputPath, _statusSuccessBrush);
      await ShowOverlayInfoAsync(
        "Logs saved",
        "Saved support logs to:\n" + outputPath + "\n\n" + nextStep);
    }

    private async Task SaveInstallWarningSupportBundleAsync(string warningDetail, InstallerResult installResult) {
      var timestamp = DateTime.Now.ToString("yyyyMMdd_HHmmss");
      var saveDialog = new SaveFileDialog {
        Title = "Save support logs",
        Filter = "Log report (*.txt)|*.txt",
        DefaultExt = ".txt",
        AddExtension = true,
        OverwritePrompt = true,
        FileName = "vibeshine-install-warnings-" + timestamp + ".txt",
        InitialDirectory = Environment.GetFolderPath(Environment.SpecialFolder.DesktopDirectory)
      };

      var selected = saveDialog.ShowDialog(this);
      if (selected != true || string.IsNullOrWhiteSpace(saveDialog.FileName)) {
        return;
      }

      string error = null;
      var outputPath = saveDialog.FileName;
      try {
        await Task.Run(() => WriteInstallWarningSupportReport(outputPath, warningDetail, installResult));
      } catch (Exception ex) {
        error = ex.Message;
      }

      if (!string.IsNullOrWhiteSpace(error)) {
        SetStatus("Could not save support logs.", error, _statusErrorBrush);
        await ShowOverlayInfoAsync("Could not save logs", error);
        return;
      }

      var nextStep = "Attach this file on GitHub: https://github.com/Nonary/Vibepollo/issues\n"
        + "Or Discord (#vibeshine): https://discord.com/invite/CGg5JxN";
      SetStatus("Support logs saved.", outputPath, _statusSuccessBrush);
      await ShowOverlayInfoAsync(
        "Logs saved",
        "Saved support logs to:\n" + outputPath + "\n\n" + nextStep);
    }

    private void WriteInstallFailureSupportReport(string outputPath, string failureDetail, InstallerResult installResult) {
      var candidateLogs = CollectSupportLogFiles(installResult == null ? null : installResult.LogPath);
      var destination = "GitHub issue or Discord #vibeshine";
      var executionVersion = _bundleVersion.ToString(3);
      using (var writer = new StreamWriter(outputPath, false)) {
        writer.WriteLine(BuildSupportSummary(destination, executionVersion, failureDetail, installResult, candidateLogs.Count, "Vibepollo install failure report", "Failure detail:"));
        writer.WriteLine();

        if (candidateLogs.Count == 0) {
          writer.WriteLine("No installer logs were found.");
          return;
        }

        foreach (var file in candidateLogs) {
          writer.WriteLine("===== BEGIN LOG: " + file + " =====");
          try {
            foreach (var line in File.ReadLines(file)) {
              writer.WriteLine(line);
            }
          } catch (Exception ex) {
            writer.WriteLine("[Could not read this log file: " + ex.Message + "]");
          }
          writer.WriteLine("===== END LOG: " + file + " =====");
          writer.WriteLine();
        }
      }
    }

    private void WriteInstallWarningSupportReport(string outputPath, string warningDetail, InstallerResult installResult) {
      var candidateLogs = CollectSupportLogFiles(installResult == null ? null : installResult.LogPath);
      var destination = "GitHub issue or Discord #vibeshine";
      var executionVersion = _bundleVersion.ToString(3);
      using (var writer = new StreamWriter(outputPath, false)) {
        writer.WriteLine(BuildSupportSummary(destination, executionVersion, warningDetail, installResult, candidateLogs.Count, "Vibepollo install warning report", "Warning detail:"));
        writer.WriteLine();

        if (candidateLogs.Count == 0) {
          writer.WriteLine("No installer logs were found.");
          return;
        }

        foreach (var file in candidateLogs) {
          writer.WriteLine("===== BEGIN LOG: " + file + " =====");
          try {
            foreach (var line in File.ReadLines(file)) {
              writer.WriteLine(line);
            }
          } catch (Exception ex) {
            writer.WriteLine("[Could not read this log file: " + ex.Message + "]");
          }
          writer.WriteLine("===== END LOG: " + file + " =====");
          writer.WriteLine();
        }
      }
    }

    private static List<string> CollectSupportLogFiles(string preferredLogPath) {
      var collected = new List<string>();
      var seen = new HashSet<string>(StringComparer.OrdinalIgnoreCase);

      TryAddLogFile(collected, seen, preferredLogPath);

      var tempPath = Path.GetTempPath();
      TryAddRecentLogs(collected, seen, tempPath, "vibeshine_install_*.log", 8);
      TryAddRecentLogs(collected, seen, tempPath, "vibeshine_preinstall_remove_*.log", 8);
      TryAddRecentLogs(collected, seen, tempPath, "vibeshine_uninstall_*.log", 4);

      var programFilesLogs = Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles),
        "Sunshine",
        "config",
        "logs");
      TryAddRecentLogs(collected, seen, programFilesLogs, "*.log", 8);

      var roamingLogs = Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData),
        "Sunshine",
        "logs");
      TryAddRecentLogs(collected, seen, roamingLogs, "*.log", 8);

      return collected;
    }

    private static void TryAddRecentLogs(List<string> collected, HashSet<string> seen, string directory, string pattern, int limit) {
      if (string.IsNullOrWhiteSpace(directory) || !Directory.Exists(directory)) {
        return;
      }

      IEnumerable<string> files;
      try {
        files = Directory
          .GetFiles(directory, pattern, SearchOption.TopDirectoryOnly)
          .OrderByDescending(path => {
            try {
              return File.GetLastWriteTimeUtc(path);
            } catch {
              return DateTime.MinValue;
            }
          })
          .Take(limit);
      } catch {
        return;
      }

      foreach (var file in files) {
        TryAddLogFile(collected, seen, file);
      }
    }

    private static void TryAddLogFile(List<string> collected, HashSet<string> seen, string path) {
      if (string.IsNullOrWhiteSpace(path)) {
        return;
      }

      string fullPath;
      try {
        fullPath = Path.GetFullPath(path);
      } catch {
        return;
      }

      if (!File.Exists(fullPath) || seen.Contains(fullPath)) {
        return;
      }

      seen.Add(fullPath);
      collected.Add(fullPath);
    }

    private static string BuildSupportSummary(
      string destination,
      string installerVersion,
      string detail,
      InstallerResult result,
      int collectedLogCount,
      string reportTitle,
      string detailLabel) {
      var lines = new List<string> {
        string.IsNullOrWhiteSpace(reportTitle) ? "Vibepollo install support report" : reportTitle,
        "Generated (UTC): " + DateTime.UtcNow.ToString("yyyy-MM-dd HH:mm:ss"),
        "Destination: " + destination,
        "Installer version: " + installerVersion,
        "Exit code: " + (result == null ? "unknown" : result.ExitCode.ToString()),
        "Operation: " + (result == null ? "install" : result.Operation.ToString()),
        "Collected logs: " + collectedLogCount,
        string.Empty,
        string.IsNullOrWhiteSpace(detailLabel) ? "Detail:" : detailLabel,
        detail ?? "Unknown error",
        string.Empty,
        "Next step:",
        "Attach this file on GitHub: https://github.com/Nonary/Vibepollo/issues",
        "Or Discord (#vibeshine): https://discord.com/invite/CGg5JxN"
      };
      return string.Join(Environment.NewLine, lines);
    }

    private TextBlock BuildSupportLinksTextBlock() {
      var block = new TextBlock {
        FontSize = 12,
        Foreground = new SolidColorBrush(Color.FromRgb(203, 219, 241)),
        Margin = new Thickness(0, 0, 0, 8),
        TextWrapping = TextWrapping.Wrap
      };

      block.Inlines.Add(new Run("Open an issue on "));
      var githubLink = new Hyperlink(new Run("GitHub")) {
        NavigateUri = new Uri("https://github.com/Nonary/Vibepollo/issues")
      };
      githubLink.Click += (sender, args) => OpenExternalUrl("https://github.com/Nonary/Vibepollo/issues");
      block.Inlines.Add(githubLink);
      block.Inlines.Add(new Run(" or join "));
      var discordLink = new Hyperlink(new Run("Discord (#vibeshine)")) {
        NavigateUri = new Uri("https://discord.com/invite/CGg5JxN")
      };
      discordLink.Click += (sender, args) => OpenExternalUrl("https://discord.com/invite/CGg5JxN");
      block.Inlines.Add(discordLink);
      block.Inlines.Add(new Run("."));

      return block;
    }

    private static void OpenExternalUrl(string url) {
      if (string.IsNullOrWhiteSpace(url)) {
        return;
      }

      try {
        Process.Start(new ProcessStartInfo {
          FileName = url,
          UseShellExecute = true
        });
      } catch {
      }
    }

    private void SetStatus(string headline, string detail, Brush headlineBrush) {
      _statusText.Text = headline;
      _statusText.Foreground = headlineBrush;
      _statusDetailText.Text = detail ?? string.Empty;
    }
  }

  internal enum InstallerOperation {
    Install,
    Uninstall
  }

  internal sealed class InstallerResult {
    public InstallerOperation Operation { get; set; }
    public int ExitCode { get; set; }
    public string Message { get; set; }
    public string UserDetail { get; set; }
    public string LogPath { get; set; }
    public List<string> ComponentFailures { get; set; }
    public bool Succeeded {
      get { return ExitCode == 0 || ExitCode == 3010; }
    }
    public bool PartiallySucceeded {
      get { return Succeeded && ComponentFailures != null && ComponentFailures.Count > 0; }
    }
  }

  internal sealed class InternalInstallResultSnapshot {
    public int ExitCode { get; set; }
    public string Message { get; set; }
    public string UserDetail { get; set; }
    public string LogPath { get; set; }
    public List<string> ComponentFailures { get; set; }
  }

  internal sealed class InstallerArguments {
    private static readonly string[] HelpTokens = { "/?", "/h", "-h", "--help" };
    private static readonly string[] UiTokens = { "--ui" };
    private static readonly string[] NoUiTokens = { "--no-ui" };
    private static readonly string[] UninstallUiTokens = { "--uninstall-ui", "--uninstall", "/uninstall" };
    private static readonly string[] QuietTokens = { "/quiet", "/qn", "/qb", "/passive" };
    private const string InternalElevatedInstallToken = "--internal-elevated-install";
    private const string InternalElevatedUninstallToken = "--internal-elevated-uninstall";
    private const string InternalInstallPathToken = "--internal-install-path";
    private const string InternalInstallSudoVdaToken = "--internal-install-sudovda";
    private const string InternalInstallSaveLogsToken = "--internal-install-save-logs";
    private const string InternalInstallResultPathToken = "--internal-install-result-path";
    private const string InternalUninstallDeleteInstallDirToken = "--internal-uninstall-delete-install-dir";
    private const string InternalUninstallRemoveSudoVdaToken = "--internal-uninstall-remove-sudovda";

    public bool ShowUi { get; set; }
    public bool UninstallUiRequested { get; set; }
    public bool InternalElevatedInstall { get; set; }
    public bool InternalElevatedUninstall { get; set; }
    public string InternalInstallPath { get; set; }
    public bool InternalInstallVirtualDisplay { get; set; }
    public bool InternalInstallSaveLogs { get; set; }
    public string InternalInstallResultPath { get; set; }
    public bool InternalUninstallDeleteInstallDir { get; set; }
    public bool InternalUninstallRemoveVirtualDisplayDriver { get; set; }
    public string MsiPathOverride { get; set; }
    public List<string> ForwardedArguments { get; private set; }

    public InstallerArguments() {
      InternalInstallVirtualDisplay = true;
      ForwardedArguments = new List<string>();
    }

    public static bool IsHelpRequested(string[] args) {
      return args.Any(arg => HelpTokens.Contains(arg, StringComparer.OrdinalIgnoreCase));
    }

    public static InstallerArguments Parse(string[] args) {
      var parsed = new InstallerArguments();
      var showUiFlag = false;
      var noUiFlag = false;

      for (var index = 0; index < args.Length; index++) {
        var arg = args[index];
        if (UiTokens.Contains(arg, StringComparer.OrdinalIgnoreCase)) {
          showUiFlag = true;
          continue;
        }
        if (NoUiTokens.Contains(arg, StringComparer.OrdinalIgnoreCase)) {
          noUiFlag = true;
          continue;
        }
        if (UninstallUiTokens.Contains(arg, StringComparer.OrdinalIgnoreCase)) {
          parsed.UninstallUiRequested = true;
          continue;
        }
        if (string.Equals(arg, InternalElevatedInstallToken, StringComparison.OrdinalIgnoreCase)) {
          parsed.InternalElevatedInstall = true;
          continue;
        }
        if (string.Equals(arg, InternalElevatedUninstallToken, StringComparison.OrdinalIgnoreCase)) {
          parsed.InternalElevatedUninstall = true;
          continue;
        }
        if (string.Equals(arg, InternalInstallPathToken, StringComparison.OrdinalIgnoreCase) && index + 1 < args.Length) {
          parsed.InternalInstallPath = args[++index];
          continue;
        }
        if (string.Equals(arg, InternalInstallSudoVdaToken, StringComparison.OrdinalIgnoreCase) && index + 1 < args.Length) {
          parsed.InternalInstallVirtualDisplay = ParseBooleanToken(args[++index]);
          continue;
        }
        if (string.Equals(arg, InternalInstallSaveLogsToken, StringComparison.OrdinalIgnoreCase) && index + 1 < args.Length) {
          parsed.InternalInstallSaveLogs = ParseBooleanToken(args[++index]);
          continue;
        }
        if (string.Equals(arg, InternalInstallResultPathToken, StringComparison.OrdinalIgnoreCase) && index + 1 < args.Length) {
          parsed.InternalInstallResultPath = args[++index];
          continue;
        }
        if (string.Equals(arg, InternalUninstallDeleteInstallDirToken, StringComparison.OrdinalIgnoreCase) && index + 1 < args.Length) {
          parsed.InternalUninstallDeleteInstallDir = ParseBooleanToken(args[++index]);
          continue;
        }
        if (string.Equals(arg, InternalUninstallRemoveSudoVdaToken, StringComparison.OrdinalIgnoreCase) && index + 1 < args.Length) {
          parsed.InternalUninstallRemoveVirtualDisplayDriver = ParseBooleanToken(args[++index]);
          continue;
        }
        if (string.Equals(arg, "--msi", StringComparison.OrdinalIgnoreCase) && index + 1 < args.Length) {
          parsed.MsiPathOverride = args[++index];
          continue;
        }
        parsed.ForwardedArguments.Add(arg);
      }

      if (showUiFlag) {
        parsed.ShowUi = true;
      } else if (noUiFlag) {
        parsed.ShowUi = false;
      } else {
        parsed.ShowUi = parsed.ForwardedArguments.Count == 0;
      }

      return parsed;
    }

    private static bool ParseBooleanToken(string value) {
      return value == "1"
        || value.Equals("true", StringComparison.OrdinalIgnoreCase)
        || value.Equals("yes", StringComparison.OrdinalIgnoreCase);
    }

    public static void WriteHelp() {
#if UNINSTALL_ONLY
      Console.WriteLine("Vibepollo Uninstaller");
      Console.WriteLine("  Self-contained graphical uninstaller for Vibepollo.");
      Console.WriteLine();
      Console.WriteLine("Usage:");
      Console.WriteLine("  uninstall.exe          Launch graphical uninstall UI");
      Console.WriteLine("  uninstall.exe /quiet   Run silent uninstall");
      Console.WriteLine();
      Console.WriteLine("Optional switches forwarded to MSI uninstall:");
      Console.WriteLine("  /quiet, /qn, /qb, /passive");
      Console.WriteLine();
      Console.WriteLine("Examples:");
      Console.WriteLine("  uninstall.exe");
      Console.WriteLine("  uninstall.exe /quiet");
#else
      Console.WriteLine("Vibepollo Installer");
      Console.WriteLine("  Self-hosted game streaming server — stream your PC to any device.");
      Console.WriteLine();
      Console.WriteLine("Usage:");
      Console.WriteLine("  VibepolloSetup.exe                Launch graphical installer UI");
      Console.WriteLine("  VibepolloSetup.exe [MSI options]  Pass options to msiexec");
      Console.WriteLine();
      Console.WriteLine("Wrapper options:");
      Console.WriteLine("  --msi <path>    Use a specific MSI payload instead of the embedded one");
      Console.WriteLine("  --ui            Force graphical mode (default when no arguments given)");
      Console.WriteLine("  --no-ui         Force command-line passthrough mode");
      Console.WriteLine("  --uninstall-ui  Open graphical UI in uninstall mode");
      Console.WriteLine("  /uninstall      Open graphical UI in uninstall mode (used by ARP)");
      Console.WriteLine("  /?, /h, --help  Show this help message");
      Console.WriteLine();
      Console.WriteLine("Supported MSI properties:");
      Console.WriteLine("  INSTALL_ROOT=<path>  Install to a custom directory (default: %ProgramFiles%\\Apollo)");
      Console.WriteLine("  INSTALL_SUDOVDA=0    Skip Virtual Display Driver installation");
      Console.WriteLine();
      Console.WriteLine("Examples:");
      Console.WriteLine("  VibepolloSetup.exe /qn");
      Console.WriteLine("  VibepolloSetup.exe /qn INSTALL_ROOT=\"D:\\Vibepollo\"");
      Console.WriteLine("  VibepolloSetup.exe /x {PRODUCT-CODE} /qn");
      Console.WriteLine("  VibepolloSetup.exe /qn INSTALL_SUDOVDA=0");
      Console.WriteLine("  VibepolloSetup.exe /uninstall");
      Console.WriteLine("  VibepolloSetup.exe /uninstall /quiet");
      Console.WriteLine("  VibepolloSetup.exe --msi C:\\temp\\Vibepollo.msi /passive");
#endif
    }

    public bool IsCliQuietMode() {
      return ForwardedArguments.Any(arg => QuietTokens.Contains(arg, StringComparer.OrdinalIgnoreCase));
    }
  }

  internal static class InstallerRunner {
    private static readonly string[] OperationTokens = {
      "/i",
      "/package",
      "/a",
      "/x",
      "/uninstall",
      "/f",
      "/update"
    };

    internal sealed class InstalledProductInfo {
      public string ProductCode { get; set; }
      public string DisplayName { get; set; }
      public Version Version { get; set; }
      public InstalledProductKind Kind { get; set; }
    }

    internal sealed class PayloadMsiInfo {
      public string ProductCode { get; set; }
      public string UpgradeCode { get; set; }
      public string VersionText { get; set; }
      public Version Version { get; set; }
      public List<string> RelatedProductCodes { get; set; }
    }

    internal sealed class LegacySunshineRegistration {
      public string DisplayName { get; set; }
      public string DisplayVersion { get; set; }
      public string UninstallString { get; set; }
      public string QuietUninstallString { get; set; }
      public string RegistryPath { get; set; }
    }

    internal enum InstalledProductKind {
      Unknown,
      Vibeshine,
      Vibepollo,
      Apollo,
      Sunshine
    }

    public static string DefaultInstallDirectory {
      get {
        return Path.Combine(
          Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles),
          "Apollo");
      }
    }

    public static InstalledProductInfo GetInstalledVibeshineProduct() {
      return GetInstalledProducts(false)
        .Where(product => product.Kind == InstalledProductKind.Vibeshine)
        .OrderByDescending(product => product.Version ?? new Version(0, 0, 0, 0))
        .FirstOrDefault();
    }

    public static InstalledProductInfo GetInstalledVibepolloProduct() {
      return GetInstalledProducts(false)
        .Where(product => product.Kind == InstalledProductKind.Vibepollo)
        .OrderByDescending(product => product.Version ?? new Version(0, 0, 0, 0))
        .FirstOrDefault();
    }

    public static InstalledProductInfo GetInstalledApolloProduct() {
      return GetInstalledProducts(true)
        .Where(product => product.Kind == InstalledProductKind.Apollo)
        .OrderByDescending(product => product.Version ?? new Version(0, 0, 0, 0))
        .FirstOrDefault();
    }

    public static List<InstalledProductInfo> GetInstalledApolloFamilyProducts() {
      var products = GetInstalledProducts(true)
        .Where(product => product.Kind == InstalledProductKind.Apollo || product.Kind == InstalledProductKind.Vibepollo)
        .ToList();

      var existingKinds = new HashSet<InstalledProductKind>(products.Select(product => product.Kind));
      foreach (var detectedKind in GetApolloFamilyKindsFromUninstallRegistry()) {
        if (existingKinds.Contains(detectedKind)) {
          continue;
        }

        products.Add(new InstalledProductInfo {
          ProductCode = detectedKind.ToString(),
          DisplayName = detectedKind == InstalledProductKind.Apollo ? "Apollo" : "Vibepollo",
          Version = null,
          Kind = detectedKind
        });
      }

      return products
        .OrderByDescending(product => product.Version ?? new Version(0, 0, 0, 0))
        .ToList();
    }

    public static InstalledProductInfo GetInstalledSunshineProduct() {
      return GetInstalledProducts(true)
        .Where(product => product.Kind == InstalledProductKind.Sunshine)
        .OrderByDescending(product => product.Version ?? new Version(0, 0, 0, 0))
        .FirstOrDefault();
    }

    public static LegacySunshineRegistration GetLegacySunshineRegistration() {
      var roots = new[] {
        Registry.LocalMachine,
        Registry.CurrentUser
      };

      var uninstallRoots = new[] {
        @"SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\Sunshine",
        @"SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\Sunshine"
      };

      foreach (var root in roots) {
        foreach (var uninstallRoot in uninstallRoots) {
          using (var key = root.OpenSubKey(uninstallRoot)) {
            if (key == null) {
              continue;
            }

            var displayName = Convert.ToString(key.GetValue("DisplayName"));
            if (!string.IsNullOrWhiteSpace(displayName) &&
                !displayName.StartsWith("Sunshine", StringComparison.OrdinalIgnoreCase)) {
              continue;
            }

            var uninstallString = Convert.ToString(key.GetValue("UninstallString"));
            var quietUninstallString = Convert.ToString(key.GetValue("QuietUninstallString"));
            if (string.IsNullOrWhiteSpace(uninstallString) && string.IsNullOrWhiteSpace(quietUninstallString)) {
              continue;
            }

            var commandForValidation = string.IsNullOrWhiteSpace(quietUninstallString)
              ? uninstallString
              : quietUninstallString;
            string executablePath;
            string uninstallArguments;
            if (!TrySplitExecutableAndArguments(commandForValidation, out executablePath, out uninstallArguments)) {
              continue;
            }

            var looksLikePath = executablePath.IndexOf('\\') >= 0 || executablePath.IndexOf('/') >= 0;
            if (looksLikePath && !File.Exists(executablePath)) {
              continue;
            }

            return new LegacySunshineRegistration {
              DisplayName = displayName ?? "Sunshine",
              DisplayVersion = Convert.ToString(key.GetValue("DisplayVersion")) ?? string.Empty,
              UninstallString = uninstallString ?? string.Empty,
              QuietUninstallString = quietUninstallString ?? string.Empty,
              RegistryPath = uninstallRoot
            };
          }
        }
      }

      return null;
    }

    public static LegacySunshineRegistration GetLegacyApolloRegistration() {
      var roots = new[] {
        Registry.LocalMachine,
        Registry.CurrentUser
      };

      var uninstallRoots = new[] {
        @"SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\Apollo",
        @"SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\Apollo"
      };

      foreach (var root in roots) {
        foreach (var uninstallRoot in uninstallRoots) {
          using (var key = root.OpenSubKey(uninstallRoot)) {
            if (key == null) {
              continue;
            }

            var displayName = Convert.ToString(key.GetValue("DisplayName"));
            if (!string.IsNullOrWhiteSpace(displayName) &&
                !displayName.StartsWith("Apollo", StringComparison.OrdinalIgnoreCase)) {
              continue;
            }

            var uninstallString = Convert.ToString(key.GetValue("UninstallString"));
            var quietUninstallString = Convert.ToString(key.GetValue("QuietUninstallString"));
            if (string.IsNullOrWhiteSpace(uninstallString) && string.IsNullOrWhiteSpace(quietUninstallString)) {
              continue;
            }

            var commandForValidation = string.IsNullOrWhiteSpace(quietUninstallString)
              ? uninstallString
              : quietUninstallString;
            string executablePath;
            string uninstallArguments;
            if (!TrySplitExecutableAndArguments(commandForValidation, out executablePath, out uninstallArguments)) {
              continue;
            }

            var looksLikePath = executablePath.IndexOf('\\') >= 0 || executablePath.IndexOf('/') >= 0;
            if (looksLikePath && !File.Exists(executablePath)) {
              continue;
            }

            return new LegacySunshineRegistration {
              DisplayName = displayName ?? "Apollo",
              DisplayVersion = Convert.ToString(key.GetValue("DisplayVersion")) ?? string.Empty,
              UninstallString = uninstallString ?? string.Empty,
              QuietUninstallString = quietUninstallString ?? string.Empty,
              RegistryPath = uninstallRoot
            };
          }
        }
      }

      return null;
    }

    public static PayloadMsiInfo TryGetPayloadMsiInfo(InstallerArguments arguments) {
      try {
        var msiPath = ResolveMsiPath(arguments == null ? null : arguments.MsiPathOverride);
        if (string.IsNullOrWhiteSpace(msiPath) || !File.Exists(msiPath)) {
          return null;
        }

        var productCode = ReadMsiProperty(msiPath, "ProductCode");
        var upgradeCode = ReadMsiProperty(msiPath, "UpgradeCode");
        var versionText = ReadMsiProperty(msiPath, "ProductVersion");
        if (string.IsNullOrWhiteSpace(productCode) && string.IsNullOrWhiteSpace(versionText) && string.IsNullOrWhiteSpace(upgradeCode)) {
          return null;
        }

        return new PayloadMsiInfo {
          ProductCode = productCode ?? string.Empty,
          UpgradeCode = upgradeCode ?? string.Empty,
          VersionText = versionText ?? string.Empty,
          Version = ParseVersion(versionText),
          RelatedProductCodes = EnumerateRelatedProducts(upgradeCode)
        };
      } catch {
        return null;
      }
    }

    private static List<InstalledProductInfo> GetInstalledProducts(bool includeSunshine) {
      var installedProducts = new List<InstalledProductInfo>();
      var uninstallRoots = new[] {
        @"SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall",
        @"SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall"
      };

      foreach (var root in uninstallRoots) {
        using (var hklmRoot = Registry.LocalMachine.OpenSubKey(root)) {
          if (hklmRoot != null) {
            CollectProductsFromRoot(hklmRoot, installedProducts, includeSunshine);
          }
        }
        using (var hkcuRoot = Registry.CurrentUser.OpenSubKey(root)) {
          if (hkcuRoot != null) {
            CollectProductsFromRoot(hkcuRoot, installedProducts, includeSunshine);
          }
        }
      }

      return installedProducts
        .GroupBy(product => product.ProductCode, StringComparer.OrdinalIgnoreCase)
        .Select(group => group.OrderByDescending(item => item.Version ?? new Version(0, 0, 0, 0)).First())
        .ToList();
    }

    private static void CollectProductsFromRoot(RegistryKey rootKey, List<InstalledProductInfo> output, bool includeSunshine) {
      foreach (var subKeyName in rootKey.GetSubKeyNames()) {
        if (string.IsNullOrWhiteSpace(subKeyName) || !LooksLikeProductCode(subKeyName)) {
          continue;
        }

        using (var productKey = rootKey.OpenSubKey(subKeyName)) {
          if (productKey == null) {
            continue;
          }

          var displayName = Convert.ToString(productKey.GetValue("DisplayName"));
          if (!IsWindowsInstallerProduct(productKey)) {
            continue;
          }

          var kind = GetInstalledProductKind(displayName);
          if (kind == InstalledProductKind.Unknown) {
            continue;
          }
          if (!includeSunshine && kind == InstalledProductKind.Sunshine) {
            continue;
          }

          var versionText = Convert.ToString(productKey.GetValue("DisplayVersion"));
          var parsedVersion = ParseVersion(versionText);

          output.Add(new InstalledProductInfo {
            ProductCode = subKeyName,
            DisplayName = displayName ?? string.Empty,
            Version = parsedVersion,
            Kind = kind
          });
        }
      }
    }

    private static bool IsWindowsInstallerProduct(RegistryKey productKey) {
      try {
        var value = productKey.GetValue("WindowsInstaller");
        if (value == null) {
          return false;
        }
        if (value is int) {
          return (int)value == 1;
        }
        if (value is string) {
          return string.Equals((string)value, "1", StringComparison.Ordinal);
        }
      } catch {
      }
      return false;
    }

    private static bool LooksLikeProductCode(string value) {
      return value.Length == 38 && value.StartsWith("{", StringComparison.Ordinal) && value.EndsWith("}", StringComparison.Ordinal);
    }

    private static InstalledProductKind GetInstalledProductKind(string displayName) {
      if (string.IsNullOrWhiteSpace(displayName)) {
        return InstalledProductKind.Unknown;
      }

      if (displayName.StartsWith("Vibeshine", StringComparison.OrdinalIgnoreCase)) {
        return InstalledProductKind.Vibeshine;
      }
      if (displayName.StartsWith("Vibepollo", StringComparison.OrdinalIgnoreCase)) {
        return InstalledProductKind.Vibepollo;
      }
      if (displayName.StartsWith("Apollo", StringComparison.OrdinalIgnoreCase)) {
        return InstalledProductKind.Apollo;
      }
      if (displayName.StartsWith("Sunshine", StringComparison.OrdinalIgnoreCase)) {
        return InstalledProductKind.Sunshine;
      }
      return InstalledProductKind.Unknown;
    }

    private static Version ParseVersion(string value) {
      if (string.IsNullOrWhiteSpace(value)) {
        return null;
      }

      Version parsed;
      if (Version.TryParse(value, out parsed)) {
        return parsed;
      }

      var numeric = new string(value.TakeWhile(ch => char.IsDigit(ch) || ch == '.').ToArray());
      if (Version.TryParse(numeric, out parsed)) {
        return parsed;
      }

      return null;
    }

    private static IEnumerable<InstalledProductKind> GetApolloFamilyKindsFromUninstallRegistry() {
      var detectedKinds = new HashSet<InstalledProductKind>();
      var uninstallRoots = new[] {
        @"SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall",
        @"SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall"
      };
      var hives = new[] {
        Registry.LocalMachine,
        Registry.CurrentUser
      };

      foreach (var hive in hives) {
        foreach (var uninstallRoot in uninstallRoots) {
          using (var rootKey = hive.OpenSubKey(uninstallRoot)) {
            if (rootKey == null) {
              continue;
            }

            foreach (var subKeyName in rootKey.GetSubKeyNames()) {
              if (string.IsNullOrWhiteSpace(subKeyName)) {
                continue;
              }

              using (var productKey = rootKey.OpenSubKey(subKeyName)) {
                if (productKey == null) {
                  continue;
                }

                var displayName = Convert.ToString(productKey.GetValue("DisplayName"));
                var kind = GetInstalledProductKind(displayName);
                if (kind == InstalledProductKind.Apollo || kind == InstalledProductKind.Vibepollo) {
                  detectedKinds.Add(kind);
                }
              }
            }
          }
        }
      }

      return detectedKinds;
    }

    private const uint MsiErrorSuccess = 0;
    private const uint MsiErrorMoreData = 234;

    [DllImport("msi.dll", CharSet = CharSet.Unicode)]
    private static extern uint MsiOpenPackageEx(string szPackagePath, uint dwOptions, out IntPtr hProduct);

    [DllImport("msi.dll", CharSet = CharSet.Unicode)]
    private static extern uint MsiGetProperty(IntPtr hInstall, string szName, StringBuilder szValueBuf, ref uint pchValueBuf);

    [DllImport("msi.dll", CharSet = CharSet.Unicode)]
    private static extern uint MsiEnumRelatedProducts(string lpUpgradeCode, uint dwReserved, uint iProductIndex, StringBuilder lpProductBuf);

    [DllImport("msi.dll", CharSet = CharSet.Unicode)]
    private static extern int MsiQueryProductState(string szProduct);

    [DllImport("msi.dll")]
    private static extern uint MsiCloseHandle(IntPtr hAny);

    private static bool IsInstalledProductCode(string productCode) {
      if (!LooksLikeProductCode(productCode)) {
        return false;
      }

      // INSTALLSTATE values considered installed: 1=Advertised, 3=Local, 4=Source, 5=Default.
      var state = MsiQueryProductState(productCode);
      return state == 1 || state == 3 || state == 4 || state == 5;
    }

    private static string ReadMsiProperty(string msiPath, string propertyName) {
      if (string.IsNullOrWhiteSpace(msiPath) || string.IsNullOrWhiteSpace(propertyName)) {
        return string.Empty;
      }

      IntPtr packageHandle;
      var openCode = MsiOpenPackageEx(msiPath, 0, out packageHandle);
      if (openCode != MsiErrorSuccess || packageHandle == IntPtr.Zero) {
        return string.Empty;
      }

      try {
        uint length = 256;
        var buffer = new StringBuilder((int)length);
        var getCode = MsiGetProperty(packageHandle, propertyName, buffer, ref length);
        if (getCode == MsiErrorMoreData) {
          length += 1;
          buffer = new StringBuilder((int)length);
          getCode = MsiGetProperty(packageHandle, propertyName, buffer, ref length);
        }
        if (getCode != MsiErrorSuccess) {
          return string.Empty;
        }

        return buffer.ToString();
      } finally {
        MsiCloseHandle(packageHandle);
      }
    }

    private static List<string> EnumerateRelatedProducts(string upgradeCode) {
      var products = new List<string>();
      if (string.IsNullOrWhiteSpace(upgradeCode)) {
        return products;
      }

      const uint noMoreItems = 259;
      uint index = 0;
      while (true) {
        var buffer = new StringBuilder(39);
        var code = MsiEnumRelatedProducts(upgradeCode, 0, index, buffer);
        if (code == noMoreItems) {
          break;
        }
        if (code != MsiErrorSuccess) {
          break;
        }

        var productCode = buffer.ToString();
        if (!string.IsNullOrWhiteSpace(productCode)) {
          products.Add(productCode);
        }
        index++;
      }

      return products;
    }

    public static InstallerResult RunInteractiveInstall(
      InstallerArguments arguments,
      string installDirectory,
      bool installVirtualDisplayDriver,
      bool saveInstallLogs,
      bool allowSelfElevation = true) {
      if (allowSelfElevation && !IsProcessElevated()) {
        return RunElevatedBootstrapperInstall(arguments, installDirectory, installVirtualDisplayDriver, saveInstallLogs);
      }

      var msiPath = ResolveMsiPath(arguments.MsiPathOverride);
      var migrationCleanupResult = RunPreinstallMigrationCleanup("preinstall", true, false);
      if (migrationCleanupResult.ExitCode != 0) {
        return new InstallerResult {
          Operation = InstallerOperation.Install,
          ExitCode = migrationCleanupResult.ExitCode,
          Message = "Install could not continue. " + migrationCleanupResult.Message,
          LogPath = migrationCleanupResult.LogPath
        };
      }

      var logPath = BuildLogPath("install");
      var args = new List<string> {
        "/i",
        msiPath,
        "/qn",
        "/norestart",
        "/l*v",
        logPath,
        CreatePropertyArgument("INSTALL_ROOT", installDirectory),
        "INSTALL_SUDOVDA=" + (installVirtualDisplayDriver ? "1" : "0"),
        "REBOOT=ReallySuppress",
        "SUPPRESSMSGBOXES=1"
      };
      TryAppendSameProductReinstallProperties(args, msiPath);

      var exitCode = RunMsiexec(args, true, false);
      exitCode = RetryInstallWithSameProductReinstallIfNeeded(exitCode, args, msiPath, true, false);
      if (exitCode != 0 && exitCode != 3010) {
        TryRecoverServiceStateAfterFailedInstall();
      }
      var componentFailures = new List<string>();
      var savedLogPath = string.Empty;
      var saveLogsWarning = string.Empty;
      var saveLogsDetail = string.Empty;
      if (saveInstallLogs) {
        try {
          savedLogPath = PersistInstallLog(logPath, installDirectory, "install");
        } catch (Exception ex) {
          saveLogsWarning = ex.Message;
        }
      }

      var resultMessage = BuildResultMessage("Install", exitCode, logPath);
      if (saveInstallLogs) {
        if (!string.IsNullOrWhiteSpace(savedLogPath)) {
          resultMessage += " Saved log copy: " + savedLogPath;
          saveLogsDetail = "Saved installer log: " + savedLogPath;
        } else if (!string.IsNullOrWhiteSpace(saveLogsWarning)) {
          resultMessage += " Could not save install log copy: " + saveLogsWarning;
          saveLogsDetail = "Could not save installer log copy: " + saveLogsWarning;
        } else {
          resultMessage += " Could not save install log copy.";
          saveLogsDetail = "Could not save installer log copy.";
        }
      }
      if (componentFailures.Count > 0) {
        var componentSummary = "Component warnings: " + string.Join(" ", componentFailures);
        resultMessage += " " + componentSummary;
        if (string.IsNullOrWhiteSpace(saveLogsDetail)) {
          saveLogsDetail = componentSummary;
        } else {
          saveLogsDetail += "\n" + componentSummary;
        }
      }

      return new InstallerResult {
        Operation = InstallerOperation.Install,
        ExitCode = exitCode,
        Message = resultMessage,
        UserDetail = saveLogsDetail,
        LogPath = logPath,
        ComponentFailures = componentFailures
      };
    }

    private static InstallerResult UninstallLegacySunshineRegistration() {
      var legacyRegistration = GetLegacySunshineRegistration();
      if (legacyRegistration == null) {
        return new InstallerResult {
          Operation = InstallerOperation.Uninstall,
          ExitCode = 0,
          Message = "No legacy Sunshine installation was found."
        };
      }

      var uninstallCommand = string.IsNullOrWhiteSpace(legacyRegistration.QuietUninstallString)
        ? legacyRegistration.UninstallString
        : legacyRegistration.QuietUninstallString;
      if (string.IsNullOrWhiteSpace(uninstallCommand)) {
        return new InstallerResult {
          Operation = InstallerOperation.Uninstall,
          ExitCode = 1603,
          Message = "Legacy Sunshine was detected, but no uninstall command was found."
        };
      }

      string executablePath;
      string uninstallArguments;
      if (!TrySplitExecutableAndArguments(uninstallCommand, out executablePath, out uninstallArguments)) {
        return new InstallerResult {
          Operation = InstallerOperation.Uninstall,
          ExitCode = 1603,
          Message = "Legacy Sunshine was detected, but the uninstall command could not be parsed."
        };
      }

      var looksLikePath = executablePath.IndexOf('\\') >= 0 || executablePath.IndexOf('/') >= 0;
      if (looksLikePath && !File.Exists(executablePath)) {
        return new InstallerResult {
          Operation = InstallerOperation.Uninstall,
          ExitCode = 0,
          Message = "Legacy Sunshine uninstall entry is stale; continuing with Vibepollo installation."
        };
      }

      if (string.IsNullOrWhiteSpace(legacyRegistration.QuietUninstallString) &&
          !IsMsiexecExecutable(executablePath) &&
          !HasQuietUninstallSwitch(uninstallArguments)) {
        uninstallArguments = string.IsNullOrWhiteSpace(uninstallArguments)
          ? "/S"
          : uninstallArguments + " /S";
      }

      int exitCode;
      try {
        var startInfo = new ProcessStartInfo {
          FileName = executablePath,
          Arguments = uninstallArguments ?? string.Empty,
          UseShellExecute = false,
          CreateNoWindow = true,
          WorkingDirectory = AppDomain.CurrentDomain.BaseDirectory
        };

        using (var process = Process.Start(startInfo)) {
          if (process == null) {
            return new InstallerResult {
              Operation = InstallerOperation.Uninstall,
              ExitCode = 1603,
              Message = "Legacy Sunshine uninstall could not be started."
            };
          }

          process.WaitForExit();
          exitCode = process.ExitCode;
        }
      } catch (Exception ex) {
        return new InstallerResult {
          Operation = InstallerOperation.Uninstall,
          ExitCode = 1603,
          Message = "Legacy Sunshine uninstall failed to launch: " + ex.Message
        };
      }

      if (exitCode != 0 && exitCode != 3010) {
        return new InstallerResult {
          Operation = InstallerOperation.Uninstall,
          ExitCode = exitCode,
          Message = BuildResultMessage("Uninstall", exitCode, string.Empty)
        };
      }

      if (!WaitForLegacySunshineRemoval(120)) {
        return new InstallerResult {
          Operation = InstallerOperation.Uninstall,
          ExitCode = 1603,
          Message = "Legacy Sunshine is still installed. Please uninstall Sunshine completely, then run the installer again."
        };
      }

      return new InstallerResult {
        Operation = InstallerOperation.Uninstall,
        ExitCode = exitCode,
        Message = BuildResultMessage("Uninstall", exitCode, string.Empty)
      };
    }

    private static bool WaitForLegacySunshineRemoval(int timeoutSeconds) {
      var timeout = timeoutSeconds <= 0 ? 1 : timeoutSeconds;
      var deadline = DateTime.UtcNow.AddSeconds(timeout);
      while (DateTime.UtcNow < deadline) {
        if (GetLegacySunshineRegistration() == null) {
          return true;
        }
        System.Threading.Thread.Sleep(1000);
      }

      return GetLegacySunshineRegistration() == null;
    }

    private static InstallerResult UninstallLegacyApolloRegistration() {
      var legacyRegistration = GetLegacyApolloRegistration();
      if (legacyRegistration == null) {
        return new InstallerResult {
          Operation = InstallerOperation.Uninstall,
          ExitCode = 0,
          Message = "No legacy Apollo installation was found."
        };
      }

      var uninstallCommand = string.IsNullOrWhiteSpace(legacyRegistration.QuietUninstallString)
        ? legacyRegistration.UninstallString
        : legacyRegistration.QuietUninstallString;
      if (string.IsNullOrWhiteSpace(uninstallCommand)) {
        return new InstallerResult {
          Operation = InstallerOperation.Uninstall,
          ExitCode = 1603,
          Message = "Legacy Apollo was detected, but no uninstall command was found."
        };
      }

      string executablePath;
      string uninstallArguments;
      if (!TrySplitExecutableAndArguments(uninstallCommand, out executablePath, out uninstallArguments)) {
        return new InstallerResult {
          Operation = InstallerOperation.Uninstall,
          ExitCode = 1603,
          Message = "Legacy Apollo was detected, but the uninstall command could not be parsed."
        };
      }

      var looksLikePath = executablePath.IndexOf('\\') >= 0 || executablePath.IndexOf('/') >= 0;
      if (looksLikePath && !File.Exists(executablePath)) {
        return new InstallerResult {
          Operation = InstallerOperation.Uninstall,
          ExitCode = 0,
          Message = "Legacy Apollo uninstall entry is stale; continuing with Vibeshine installation."
        };
      }

      if (string.IsNullOrWhiteSpace(legacyRegistration.QuietUninstallString) &&
          !IsMsiexecExecutable(executablePath) &&
          !HasQuietUninstallSwitch(uninstallArguments)) {
        uninstallArguments = string.IsNullOrWhiteSpace(uninstallArguments)
          ? "/S"
          : uninstallArguments + " /S";
      }

      int exitCode;
      try {
        var startInfo = new ProcessStartInfo {
          FileName = executablePath,
          Arguments = uninstallArguments ?? string.Empty,
          UseShellExecute = false,
          CreateNoWindow = true,
          WorkingDirectory = AppDomain.CurrentDomain.BaseDirectory
        };

        using (var process = Process.Start(startInfo)) {
          if (process == null) {
            return new InstallerResult {
              Operation = InstallerOperation.Uninstall,
              ExitCode = 1603,
              Message = "Legacy Apollo uninstall could not be started."
            };
          }

          process.WaitForExit();
          exitCode = process.ExitCode;
        }
      } catch (Exception ex) {
        return new InstallerResult {
          Operation = InstallerOperation.Uninstall,
          ExitCode = 1603,
          Message = "Legacy Apollo uninstall failed to launch: " + ex.Message
        };
      }

      if (exitCode != 0 && exitCode != 3010) {
        return new InstallerResult {
          Operation = InstallerOperation.Uninstall,
          ExitCode = exitCode,
          Message = BuildResultMessage("Uninstall", exitCode, string.Empty)
        };
      }

      if (!WaitForLegacyApolloRemoval(120)) {
        return new InstallerResult {
          Operation = InstallerOperation.Uninstall,
          ExitCode = 1603,
          Message = "Legacy Apollo is still installed. Please uninstall Apollo completely, then run the installer again."
        };
      }

      return new InstallerResult {
        Operation = InstallerOperation.Uninstall,
        ExitCode = exitCode,
        Message = BuildResultMessage("Uninstall", exitCode, string.Empty)
      };
    }

    private static bool WaitForLegacyApolloRemoval(int timeoutSeconds) {
      var timeout = timeoutSeconds <= 0 ? 1 : timeoutSeconds;
      var deadline = DateTime.UtcNow.AddSeconds(timeout);
      while (DateTime.UtcNow < deadline) {
        if (GetLegacyApolloRegistration() == null) {
          return true;
        }
        System.Threading.Thread.Sleep(1000);
      }

      return GetLegacyApolloRegistration() == null;
    }

    private static bool TrySplitExecutableAndArguments(string commandLine, out string executablePath, out string arguments) {
      executablePath = string.Empty;
      arguments = string.Empty;
      var value = Environment.ExpandEnvironmentVariables(commandLine ?? string.Empty).Trim();
      if (value.Length == 0) {
        return false;
      }

      if (value.StartsWith("\"", StringComparison.Ordinal)) {
        var closingQuote = value.IndexOf('"', 1);
        if (closingQuote <= 1) {
          return false;
        }

        executablePath = value.Substring(1, closingQuote - 1).Trim();
        arguments = value.Substring(closingQuote + 1).Trim();
        return executablePath.Length > 0;
      }

      var exeIndex = value.IndexOf(".exe", StringComparison.OrdinalIgnoreCase);
      if (exeIndex > 0) {
        executablePath = value.Substring(0, exeIndex + 4).Trim();
        arguments = value.Substring(exeIndex + 4).Trim();
        return executablePath.Length > 0;
      }

      var firstSpace = value.IndexOfAny(new[] { ' ', '\t' });
      if (firstSpace < 0) {
        executablePath = value;
        return true;
      }

      executablePath = value.Substring(0, firstSpace).Trim();
      arguments = value.Substring(firstSpace + 1).Trim();
      return executablePath.Length > 0;
    }

    private static bool IsMsiexecExecutable(string executablePath) {
      if (string.IsNullOrWhiteSpace(executablePath)) {
        return false;
      }

      var fileName = Path.GetFileName(executablePath).ToLowerInvariant();
      return fileName == "msiexec.exe";
    }

    private static bool HasQuietUninstallSwitch(string uninstallArguments) {
      if (string.IsNullOrWhiteSpace(uninstallArguments)) {
        return false;
      }

      var tokens = uninstallArguments
        .Split(new[] { ' ', '\t' }, StringSplitOptions.RemoveEmptyEntries)
        .Select(token => token.Trim('"', '\''))
        .ToArray();
      return tokens.Any(token =>
        token.Equals("/quiet", StringComparison.OrdinalIgnoreCase) ||
        token.Equals("/qn", StringComparison.OrdinalIgnoreCase) ||
        token.Equals("/qb", StringComparison.OrdinalIgnoreCase) ||
        token.Equals("/passive", StringComparison.OrdinalIgnoreCase) ||
        token.Equals("/s", StringComparison.OrdinalIgnoreCase) ||
        token.Equals("/silent", StringComparison.OrdinalIgnoreCase));
    }

    public static InstallerResult RunInteractiveUninstall(
      InstallerArguments arguments,
      bool deleteInstallDirectory = false,
      bool removeVirtualDisplayDriver = false,
      bool allowSelfElevation = true) {
      if (allowSelfElevation && !IsProcessElevated()) {
        return RunElevatedBootstrapperUninstall(arguments, deleteInstallDirectory, removeVirtualDisplayDriver);
      }

      var uninstallResult = UninstallInstalledProducts(
        "uninstall",
        true,
        false,
        deleteInstallDirectory,
        removeVirtualDisplayDriver,
        true,
        new[] { InstalledProductKind.Vibepollo });
      uninstallResult.Operation = InstallerOperation.Uninstall;
      return uninstallResult;
    }

    public static InstallerResult RunCli(InstallerArguments arguments) {
      var cliArgs = new List<string>(arguments.ForwardedArguments);
      var hasOperation = cliArgs.Any(IsOperationSwitch);
      var installMsiPath = string.Empty;

      if (!hasOperation) {
        installMsiPath = ResolveMsiPath(arguments.MsiPathOverride);
        cliArgs.Insert(0, installMsiPath);
        cliArgs.Insert(0, "/i");
      } else {
        TryInjectDefaultMsi(cliArgs, arguments);
        installMsiPath = TryResolveInstallMsiPath(cliArgs);
      }

      if (!string.IsNullOrWhiteSpace(installMsiPath)) {
        var migrationCleanupResult = RunPreinstallMigrationCleanup("preinstall_cli", arguments.IsCliQuietMode(), true);
        if (migrationCleanupResult.ExitCode != 0) {
          return new InstallerResult {
            Operation = InstallerOperation.Install,
            ExitCode = migrationCleanupResult.ExitCode,
            Message = "Install could not continue. " + migrationCleanupResult.Message,
            LogPath = migrationCleanupResult.LogPath
          };
        }
        TryAppendSameProductReinstallProperties(cliArgs, installMsiPath);
      }

      if (!HasRestartBehavior(cliArgs)) {
        cliArgs.Add("/norestart");
      }
      if (!HasProperty(cliArgs, "REBOOT")) {
        cliArgs.Add("REBOOT=ReallySuppress");
      }
      if (!HasProperty(cliArgs, "SUPPRESSMSGBOXES")) {
        cliArgs.Add("SUPPRESSMSGBOXES=1");
      }

      var logPath = string.Empty;
      if (!HasLogSwitch(cliArgs)) {
        logPath = BuildLogPath("cli");
        cliArgs.Add("/l*v");
        cliArgs.Add(logPath);
      }

      var exitCode = RunMsiexec(cliArgs, arguments.IsCliQuietMode(), true);
      exitCode = RetryInstallWithSameProductReinstallIfNeeded(
        exitCode,
        cliArgs,
        installMsiPath,
        arguments.IsCliQuietMode(),
        true);
      if (exitCode != 0 && exitCode != 3010) {
        TryRecoverServiceStateAfterFailedInstall();
      }
      return new InstallerResult {
        Operation = InstallerOperation.Install,
        ExitCode = exitCode,
        Message = BuildResultMessage("CLI operation", exitCode, logPath),
        LogPath = logPath
      };
    }

    private static InstallerResult RunPreinstallMigrationCleanup(
      string logPhase,
      bool hiddenWindow,
      bool requestElevationIfNeeded) {
      var migrationKinds = new HashSet<InstalledProductKind> {
        InstalledProductKind.Sunshine,
        InstalledProductKind.Vibeshine,
        InstalledProductKind.Apollo
      };
      var hasMsiMigrationTarget = GetInstalledProducts(true)
        .Any(product => migrationKinds.Contains(product.Kind));
      var hasLegacySunshineRegistration = GetLegacySunshineRegistration() != null;
      var hasLegacyApolloRegistration = GetLegacyApolloRegistration() != null;
      if (!hasMsiMigrationTarget && !hasLegacySunshineRegistration && !hasLegacyApolloRegistration) {
        return new InstallerResult {
          Operation = InstallerOperation.Uninstall,
          ExitCode = 0,
          Message = "No preinstall migration cleanup is required."
        };
      }

      TryDrainPreinstallLocks();

      var restartRequired = false;
      var cleanupLogPath = string.Empty;
      if (hasMsiMigrationTarget) {
        var migrationUninstallResult = UninstallInstalledProducts(
          logPhase,
          hiddenWindow,
          requestElevationIfNeeded,
          false,
          false,
          false,
          new[] {
            InstalledProductKind.Sunshine,
            InstalledProductKind.Vibeshine,
            InstalledProductKind.Apollo
          });
        if (migrationUninstallResult.ExitCode != 0 && migrationUninstallResult.ExitCode != 3010) {
          return migrationUninstallResult;
        }
        if (!string.IsNullOrWhiteSpace(migrationUninstallResult.LogPath)) {
          cleanupLogPath = migrationUninstallResult.LogPath;
        }
        if (migrationUninstallResult.ExitCode == 3010) {
          restartRequired = true;
        }
      }

      if (hasLegacySunshineRegistration) {
        var legacySunshineRegistrationResult = UninstallLegacySunshineRegistration();
        if (legacySunshineRegistrationResult.ExitCode != 0 && legacySunshineRegistrationResult.ExitCode != 3010) {
          return legacySunshineRegistrationResult;
        }
        if (!string.IsNullOrWhiteSpace(legacySunshineRegistrationResult.LogPath)) {
          cleanupLogPath = legacySunshineRegistrationResult.LogPath;
        }
        if (legacySunshineRegistrationResult.ExitCode == 3010) {
          restartRequired = true;
        }
      }

      if (hasLegacyApolloRegistration) {
        var legacyApolloRegistrationResult = UninstallLegacyApolloRegistration();
        if (legacyApolloRegistrationResult.ExitCode != 0 && legacyApolloRegistrationResult.ExitCode != 3010) {
          return legacyApolloRegistrationResult;
        }
        if (!string.IsNullOrWhiteSpace(legacyApolloRegistrationResult.LogPath)) {
          cleanupLogPath = legacyApolloRegistrationResult.LogPath;
        }
        if (legacyApolloRegistrationResult.ExitCode == 3010) {
          restartRequired = true;
        }
      }

      if (restartRequired) {
        return new InstallerResult {
          Operation = InstallerOperation.Uninstall,
          ExitCode = 3010,
          Message = "Migration cleanup completed and requires a reboot before installation can continue.",
          LogPath = cleanupLogPath
        };
      }

      return new InstallerResult {
        Operation = InstallerOperation.Uninstall,
        ExitCode = 0,
        Message = "Preinstall migration cleanup succeeded.",
        LogPath = cleanupLogPath
      };
    }

    private static readonly string[] PreinstallServiceNames = {
      "SunshineService",
      "sunshinesvc"
    };

    private static readonly string[] PostInstallServiceNames = {
      "SunshineService",
      "VibeshineService",
      "sunshinesvc"
    };

    private static readonly string[] PreinstallProcessNames = {
      "vibeshine",
      "sunshine",
      "sunshinesvc",
      "apollo",
      "vibepollo"
    };

    private static void TryDrainPreinstallLocks() {
      foreach (var serviceName in PreinstallServiceNames) {
        TryStopServiceAndWait(serviceName);
      }

      foreach (var processName in PreinstallProcessNames) {
        TryKillProcessesByName(processName);
      }
    }

    private static void TryKillProcessesByName(string processName) {
      if (string.IsNullOrWhiteSpace(processName)) {
        return;
      }

      Process[] processes;
      try {
        processes = Process.GetProcessesByName(processName);
      } catch {
        return;
      }

      foreach (var process in processes) {
        try {
          if (process.HasExited) {
            continue;
          }
          process.Kill();
          process.WaitForExit(5000);
        } catch {
        } finally {
          process.Dispose();
        }
      }
    }

    private static void TryRunUtilityProcess(string executable, string arguments) {
      if (string.IsNullOrWhiteSpace(executable)) {
        return;
      }

      try {
        var startInfo = new ProcessStartInfo {
          FileName = executable,
          Arguments = arguments ?? string.Empty,
          UseShellExecute = false,
          CreateNoWindow = true,
          WorkingDirectory = AppDomain.CurrentDomain.BaseDirectory
        };

        using (var process = Process.Start(startInfo)) {
          if (process == null) {
            return;
          }
          if (!process.WaitForExit(10000)) {
            try {
              process.Kill();
              process.WaitForExit(5000);
            } catch {
            }
          }
        }
      } catch {
      }
    }

    private static void TryStopServiceAndWait(string serviceName) {
      if (string.IsNullOrWhiteSpace(serviceName)) {
        return;
      }

      TryRunUtilityProcess("net.exe", "stop " + serviceName);
      WaitForServiceStateStopped(serviceName, TimeSpan.FromSeconds(15));
    }

    private static void TryRecoverServiceStateAfterFailedInstall() {
      foreach (var serviceName in PostInstallServiceNames) {
        TryStartServiceAndWait(serviceName);
      }
    }

    private static void TryStartServiceAndWait(string serviceName) {
      if (string.IsNullOrWhiteSpace(serviceName)) {
        return;
      }

      TryRunUtilityProcess("net.exe", "start " + serviceName);
      WaitForServiceStateRunning(serviceName, TimeSpan.FromSeconds(15));
    }

    private static void WaitForServiceStateStopped(string serviceName, TimeSpan timeout) {
      if (string.IsNullOrWhiteSpace(serviceName) || timeout <= TimeSpan.Zero) {
        return;
      }

      var deadline = DateTime.UtcNow + timeout;
      while (DateTime.UtcNow < deadline) {
        if (IsServiceStopped(serviceName)) {
          return;
        }
        System.Threading.Thread.Sleep(500);
      }
    }

    private static void WaitForServiceStateRunning(string serviceName, TimeSpan timeout) {
      if (string.IsNullOrWhiteSpace(serviceName) || timeout <= TimeSpan.Zero) {
        return;
      }

      var deadline = DateTime.UtcNow + timeout;
      while (DateTime.UtcNow < deadline) {
        if (IsServiceRunning(serviceName)) {
          return;
        }
        System.Threading.Thread.Sleep(500);
      }
    }

    private static bool IsServiceStopped(string serviceName) {
      try {
        var startInfo = new ProcessStartInfo {
          FileName = "sc.exe",
          Arguments = "query " + serviceName,
          UseShellExecute = false,
          CreateNoWindow = true,
          RedirectStandardOutput = true,
          RedirectStandardError = true,
          WorkingDirectory = AppDomain.CurrentDomain.BaseDirectory
        };

        using (var process = Process.Start(startInfo)) {
          if (process == null) {
            return false;
          }
          var output = process.StandardOutput.ReadToEnd();
          process.WaitForExit(5000);
          return output.IndexOf("STATE", StringComparison.OrdinalIgnoreCase) >= 0 &&
                 output.IndexOf("STOPPED", StringComparison.OrdinalIgnoreCase) >= 0;
        }
      } catch {
        return false;
      }
    }

    private static bool IsServiceRunning(string serviceName) {
      try {
        var startInfo = new ProcessStartInfo {
          FileName = "sc.exe",
          Arguments = "query " + serviceName,
          UseShellExecute = false,
          CreateNoWindow = true,
          RedirectStandardOutput = true,
          RedirectStandardError = true,
          WorkingDirectory = AppDomain.CurrentDomain.BaseDirectory
        };

        using (var process = Process.Start(startInfo)) {
          if (process == null) {
            return false;
          }
          var output = process.StandardOutput.ReadToEnd();
          process.WaitForExit(5000);
          return output.IndexOf("STATE", StringComparison.OrdinalIgnoreCase) >= 0 &&
                 output.IndexOf("RUNNING", StringComparison.OrdinalIgnoreCase) >= 0;
        }
      } catch {
        return false;
      }
    }

    private static InstallerResult UninstallInstalledProducts(
      string logPhase,
      bool hiddenWindow,
      bool requestElevationIfNeeded,
      bool deleteInstallDirectory,
      bool removeVirtualDisplayDriver,
      bool failWhenMissing,
      IReadOnlyCollection<InstalledProductKind> uninstallKinds) {
      var kinds = uninstallKinds ?? Array.Empty<InstalledProductKind>();
      var installedProducts = GetInstalledProducts(true)
        .Where(product => kinds.Count == 0 || kinds.Contains(product.Kind))
        .ToList();
      if (installedProducts.Count == 0) {
        var missingMessage = "No matching installations were found.";
        if (kinds.Count == 1) {
          var singleKind = kinds.First();
          missingMessage = "No existing " + singleKind + " installation was found.";
        }
        return new InstallerResult {
          Operation = InstallerOperation.Uninstall,
          ExitCode = failWhenMissing ? 1605 : 0,
          Message = missingMessage
        };
      }

      var finalCode = 0;
      var lastLogPath = string.Empty;
      foreach (var product in installedProducts) {
        var logPath = BuildLogPath(logPhase + "_remove");
        lastLogPath = logPath;

        var args = new List<string> {
          "/x",
          product.ProductCode,
          "/qn",
          "/norestart",
          "/l*v",
          logPath,
          "DELETEINSTALLDIR=" + (deleteInstallDirectory ? "1" : "0"),
          "REMOVEVIRTUALDISPLAYDRIVER=" + (removeVirtualDisplayDriver ? "1" : "0"),
          "REBOOT=ReallySuppress",
          "SUPPRESSMSGBOXES=1"
        };

        var code = RunMsiexec(args, hiddenWindow, requestElevationIfNeeded);
        if (code == 3010) {
          finalCode = 3010;
          continue;
        }
        if (code == 0 || code == 1605) {
          continue;
        }

        return new InstallerResult {
          Operation = InstallerOperation.Uninstall,
          ExitCode = code,
          Message = BuildResultMessage("Uninstall", code, logPath),
          LogPath = logPath
        };
      }

      return new InstallerResult {
        Operation = InstallerOperation.Uninstall,
        ExitCode = finalCode,
        Message = BuildResultMessage("Uninstall", finalCode, lastLogPath),
        LogPath = lastLogPath
      };
    }

    private static string ResolveMsiPath(string overridePath) {
      if (!string.IsNullOrWhiteSpace(overridePath)) {
        var explicitPath = Path.GetFullPath(overridePath);
        if (!File.Exists(explicitPath)) {
          throw new FileNotFoundException("Specified MSI payload was not found.", explicitPath);
        }
        return explicitPath;
      }

      // Prefer the embedded payload to avoid stale sidecar MSI files overriding the
      // version and install target unexpectedly. Sidecar remains a fallback.
      try {
        return ExtractEmbeddedMsi();
      } catch {
        var sidecarMsi = FindSidecarMsi();
        if (!string.IsNullOrWhiteSpace(sidecarMsi)) {
          return sidecarMsi;
        }
        throw;
      }
    }

    private static string FindSidecarMsi() {
      var baseDirectory = AppDomain.CurrentDomain.BaseDirectory;
      var msiFiles = Directory.Exists(baseDirectory)
        ? Directory.GetFiles(baseDirectory, "*.msi")
        : new string[0];
      return msiFiles
        .OrderByDescending(File.GetLastWriteTimeUtc)
        .FirstOrDefault();
    }

    private static string ExtractEmbeddedMsi() {
      using (var stream = Assembly.GetExecutingAssembly().GetManifestResourceStream("Payload.msi")) {
        if (stream == null) {
          throw new InvalidOperationException(
            "No MSI payload was found. The installer may be corrupted.\n\n"
            + "Try re-downloading the installer from the Vibepollo releases page, "
            + "or use the --msi option to specify a payload manually.");
        }

        var versionToken = ComputeStreamSha256Hex(stream);
        var extractDirectory = Path.Combine(
          Path.GetTempPath(),
          "VibepolloInstaller",
          versionToken);
        Directory.CreateDirectory(extractDirectory);

        var msiPath = Path.Combine(extractDirectory, "Vibepollo.msi");
        var shouldWrite = !File.Exists(msiPath) || new FileInfo(msiPath).Length != stream.Length;
        if (shouldWrite) {
          stream.Position = 0;
          using (var output = File.Create(msiPath)) {
            stream.CopyTo(output);
          }
        }

        return msiPath;
      }
    }

    private static string ComputeStreamSha256Hex(Stream stream) {
      if (stream == null) {
        return "unknown";
      }

      if (stream.CanSeek) {
        var originalPosition = stream.Position;
        try {
          using (var hasher = SHA256.Create()) {
            var hash = hasher.ComputeHash(stream);
            return string.Concat(hash.Select(b => b.ToString("x2")));
          }
        } finally {
          stream.Position = originalPosition;
        }
      }

      using (var hasher = SHA256.Create()) {
        var hash = hasher.ComputeHash(stream);
        return string.Concat(hash.Select(b => b.ToString("x2")));
      }
    }

    private static bool IsOperationSwitch(string value) {
      return OperationTokens.Contains(value, StringComparer.OrdinalIgnoreCase);
    }

    private static void TryInjectDefaultMsi(List<string> cliArgs, InstallerArguments arguments) {
      var operationIndex = cliArgs.FindIndex(IsOperationSwitch);
      if (operationIndex < 0) {
        return;
      }

      var operation = cliArgs[operationIndex];
      var hasValueAfterOperation = operationIndex + 1 < cliArgs.Count && !LooksLikeSwitch(cliArgs[operationIndex + 1]);
      if (hasValueAfterOperation) {
        return;
      }

      if (!string.Equals(operation, "/i", StringComparison.OrdinalIgnoreCase) &&
          !string.Equals(operation, "/package", StringComparison.OrdinalIgnoreCase) &&
          !string.Equals(operation, "/a", StringComparison.OrdinalIgnoreCase) &&
          !string.Equals(operation, "/x", StringComparison.OrdinalIgnoreCase)) {
        return;
      }

      cliArgs.Insert(operationIndex + 1, ResolveMsiPath(arguments.MsiPathOverride));
    }

    private static bool LooksLikeSwitch(string value) {
      if (string.IsNullOrWhiteSpace(value)) {
        return true;
      }
      if (value.StartsWith("{", StringComparison.Ordinal) && value.EndsWith("}", StringComparison.Ordinal)) {
        return false;
      }
      if (value.Length >= 2 && value[1] == ':') {
        return false;
      }
      return value.StartsWith("/", StringComparison.Ordinal) || value.StartsWith("-", StringComparison.Ordinal);
    }

    private static bool HasRestartBehavior(List<string> args) {
      return args.Any(arg =>
        string.Equals(arg, "/norestart", StringComparison.OrdinalIgnoreCase) ||
        string.Equals(arg, "/promptrestart", StringComparison.OrdinalIgnoreCase) ||
        string.Equals(arg, "/forcerestart", StringComparison.OrdinalIgnoreCase));
    }

    private static bool HasProperty(List<string> args, string propertyName) {
      var prefix = propertyName + "=";
      return args.Any(arg => arg.StartsWith(prefix, StringComparison.OrdinalIgnoreCase));
    }

    private static int RetryInstallWithSameProductReinstallIfNeeded(
      int exitCode,
      List<string> args,
      string msiPath,
      bool hiddenWindow,
      bool requestElevationIfNeeded) {
      const int anotherVersionInstalled = 1638;
      if (exitCode != anotherVersionInstalled || args == null) {
        return exitCode;
      }
      if (HasProperty(args, "REINSTALL") && HasProperty(args, "REINSTALLMODE")) {
        return exitCode;
      }
      if (string.IsNullOrWhiteSpace(msiPath) || !File.Exists(msiPath)) {
        return exitCode;
      }

      var retryArgs = new List<string>(args);
      if (!HasProperty(retryArgs, "REINSTALL")) {
        retryArgs.Add("REINSTALL=ALL");
      }
      if (!HasProperty(retryArgs, "REINSTALLMODE")) {
        retryArgs.Add("REINSTALLMODE=vams");
      }
      return RunMsiexec(retryArgs, hiddenWindow, requestElevationIfNeeded);
    }

    private static void TryAppendSameProductReinstallProperties(List<string> args, string msiPath) {
      if (args == null || string.IsNullOrWhiteSpace(msiPath)) {
        return;
      }
      if (!ShouldUseSameProductReinstall(msiPath)) {
        return;
      }

      if (!HasProperty(args, "REINSTALL")) {
        args.Add("REINSTALL=ALL");
      }
      if (!HasProperty(args, "REINSTALLMODE")) {
        // Use vams (no 'u') to preserve existing HKCU settings during same-product reinstalls.
        args.Add("REINSTALLMODE=vams");
      }
    }

    private static bool ShouldUseSameProductReinstall(string msiPath) {
      if (string.IsNullOrWhiteSpace(msiPath) || !File.Exists(msiPath)) {
        return false;
      }

      var payloadInfo = TryGetPayloadMsiInfo(new InstallerArguments {
        MsiPathOverride = msiPath
      });
      if (payloadInfo == null || string.IsNullOrWhiteSpace(payloadInfo.ProductCode)) {
        return false;
      }

      if (IsInstalledProductCode(payloadInfo.ProductCode)) {
        return true;
      }

      return GetInstalledProducts(true).Any(product =>
        !string.IsNullOrWhiteSpace(product.ProductCode) &&
        string.Equals(product.ProductCode, payloadInfo.ProductCode, StringComparison.OrdinalIgnoreCase));
    }

    private static string TryResolveInstallMsiPath(List<string> cliArgs) {
      if (cliArgs == null || cliArgs.Count == 0) {
        return string.Empty;
      }

      var operationIndex = cliArgs.FindIndex(IsOperationSwitch);
      if (operationIndex < 0 || operationIndex + 1 >= cliArgs.Count) {
        return string.Empty;
      }

      var operation = cliArgs[operationIndex];
      if (!string.Equals(operation, "/i", StringComparison.OrdinalIgnoreCase) &&
          !string.Equals(operation, "/package", StringComparison.OrdinalIgnoreCase)) {
        return string.Empty;
      }

      var candidate = cliArgs[operationIndex + 1];
      if (string.IsNullOrWhiteSpace(candidate) ||
          LooksLikeSwitch(candidate) ||
          (candidate.StartsWith("{", StringComparison.Ordinal) && candidate.EndsWith("}", StringComparison.Ordinal))) {
        return string.Empty;
      }

      try {
        var fullPath = Path.GetFullPath(candidate);
        if (File.Exists(fullPath)) {
          return fullPath;
        }
      } catch {
      }

      return string.Empty;
    }

    private static bool HasLogSwitch(List<string> args) {
      return args.Any(arg =>
        arg.StartsWith("/l", StringComparison.OrdinalIgnoreCase) ||
        string.Equals(arg, "/log", StringComparison.OrdinalIgnoreCase));
    }

    private static string BuildLogPath(string phase) {
      var timestamp = DateTime.UtcNow.ToString("yyyyMMdd_HHmmss");
      return Path.Combine(Path.GetTempPath(), "vibeshine_" + phase + "_" + timestamp + ".log");
    }

    private static string PersistInstallLog(string sourceLogPath, string installDirectory, string phase) {
      if (string.IsNullOrWhiteSpace(sourceLogPath) || !File.Exists(sourceLogPath)) {
        throw new InvalidOperationException("The install log was not found in the temporary folder.");
      }
      if (string.IsNullOrWhiteSpace(installDirectory)) {
        throw new InvalidOperationException("Install directory is not available.");
      }

      var fullInstallDirectory = Path.GetFullPath(installDirectory);
      var logDirectory = Path.Combine(fullInstallDirectory, "config", "logs", "installer");
      Directory.CreateDirectory(logDirectory);

      var timestamp = DateTime.UtcNow.ToString("yyyyMMdd_HHmmss");
      var destinationFileName = "vibeshine_" + phase + "_" + timestamp + ".log";
      var destinationPath = Path.Combine(logDirectory, destinationFileName);
      File.Copy(sourceLogPath, destinationPath, true);
      return destinationPath;
    }

    private static InstallerResult RunElevatedBootstrapperInstall(
      InstallerArguments arguments,
      string installDirectory,
      bool installVirtualDisplayDriver,
      bool saveInstallLogs) {
      var resultPath = Path.Combine(Path.GetTempPath(), "vibeshine_install_result_" + Guid.NewGuid().ToString("N") + ".txt");
      var elevatedArgs = new List<string> {
        "--internal-elevated-install",
        "--internal-install-path",
        installDirectory,
        "--internal-install-sudovda",
        installVirtualDisplayDriver ? "1" : "0",
        "--internal-install-save-logs",
        saveInstallLogs ? "1" : "0",
        "--internal-install-result-path",
        resultPath
      };
      if (!string.IsNullOrWhiteSpace(arguments.MsiPathOverride)) {
        elevatedArgs.Add("--msi");
        elevatedArgs.Add(arguments.MsiPathOverride);
      }

      var exitCode = RunElevatedBootstrapper(elevatedArgs);
      var snapshot = TryReadInternalInstallResult(resultPath);
      var installLogPath = FindMostRecentLog(Path.GetTempPath(), "vibeshine_install_*.log");
      if (snapshot != null && !string.IsNullOrWhiteSpace(snapshot.LogPath)) {
        installLogPath = snapshot.LogPath;
      }
      TryDeleteFile(resultPath);
      return new InstallerResult {
        Operation = InstallerOperation.Install,
        ExitCode = exitCode,
        Message = snapshot == null
          ? BuildResultMessage("Install", exitCode, installLogPath)
          : string.IsNullOrWhiteSpace(snapshot.Message)
            ? BuildResultMessage("Install", exitCode, installLogPath)
            : snapshot.Message,
        UserDetail = snapshot == null ? string.Empty : snapshot.UserDetail,
        LogPath = installLogPath,
        ComponentFailures = snapshot == null ? new List<string>() : (snapshot.ComponentFailures ?? new List<string>())
      };
    }

    private static InstallerResult RunElevatedBootstrapperUninstall(
      InstallerArguments arguments,
      bool deleteInstallDirectory,
      bool removeVirtualDisplayDriver) {
      var elevatedArgs = new List<string> {
        "--internal-elevated-uninstall",
        "--internal-uninstall-delete-install-dir",
        deleteInstallDirectory ? "1" : "0",
        "--internal-uninstall-remove-sudovda",
        removeVirtualDisplayDriver ? "1" : "0"
      };
      if (!string.IsNullOrWhiteSpace(arguments.MsiPathOverride)) {
        elevatedArgs.Add("--msi");
        elevatedArgs.Add(arguments.MsiPathOverride);
      }

      var exitCode = RunElevatedBootstrapper(elevatedArgs);
      var uninstallLogPath = FindMostRecentLog(Path.GetTempPath(), "vibeshine_uninstall_*.log")
        ?? FindMostRecentLog(Path.GetTempPath(), "vibeshine_uninstall_remove_*.log");
      return new InstallerResult {
        Operation = InstallerOperation.Uninstall,
        ExitCode = exitCode,
        Message = BuildResultMessage("Uninstall", exitCode, uninstallLogPath),
        LogPath = uninstallLogPath
      };
    }

    private static string FindMostRecentLog(string directory, string pattern) {
      if (string.IsNullOrWhiteSpace(directory) || !Directory.Exists(directory)) {
        return null;
      }

      try {
        return Directory
          .GetFiles(directory, pattern, SearchOption.TopDirectoryOnly)
          .OrderByDescending(File.GetLastWriteTimeUtc)
          .FirstOrDefault();
      } catch {
        return null;
      }
    }

    internal static void TryWriteInternalInstallResult(string resultPath, InstallerResult result) {
      if (string.IsNullOrWhiteSpace(resultPath) || result == null) {
        return;
      }

      try {
        var failures = result.ComponentFailures == null
          ? string.Empty
          : string.Join("\n", result.ComponentFailures.Where(item => !string.IsNullOrWhiteSpace(item)));
        var lines = new[] {
          "ExitCode=" + result.ExitCode,
          "MessageB64=" + Convert.ToBase64String(Encoding.UTF8.GetBytes(result.Message ?? string.Empty)),
          "UserDetailB64=" + Convert.ToBase64String(Encoding.UTF8.GetBytes(result.UserDetail ?? string.Empty)),
          "LogPathB64=" + Convert.ToBase64String(Encoding.UTF8.GetBytes(result.LogPath ?? string.Empty)),
          "ComponentFailuresB64=" + Convert.ToBase64String(Encoding.UTF8.GetBytes(failures))
        };
        File.WriteAllLines(resultPath, lines, Encoding.UTF8);
      } catch {
      }
    }

    private static InternalInstallResultSnapshot TryReadInternalInstallResult(string resultPath) {
      if (string.IsNullOrWhiteSpace(resultPath) || !File.Exists(resultPath)) {
        return null;
      }

      try {
        var map = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
        foreach (var line in File.ReadAllLines(resultPath, Encoding.UTF8)) {
          if (string.IsNullOrWhiteSpace(line)) {
            continue;
          }
          var splitIndex = line.IndexOf('=');
          if (splitIndex <= 0) {
            continue;
          }
          var key = line.Substring(0, splitIndex);
          var value = splitIndex + 1 < line.Length ? line.Substring(splitIndex + 1) : string.Empty;
          map[key] = value;
        }

        int parsedExitCode;
        if (!int.TryParse(map.ContainsKey("ExitCode") ? map["ExitCode"] : "0", out parsedExitCode)) {
          parsedExitCode = 0;
        }

        var failures = DecodeBase64Utf8(map, "ComponentFailuresB64")
          .Split(new[] { "\r\n", "\n" }, StringSplitOptions.RemoveEmptyEntries)
          .Select(item => item.Trim())
          .Where(item => item.Length > 0)
          .ToList();

        return new InternalInstallResultSnapshot {
          ExitCode = parsedExitCode,
          Message = DecodeBase64Utf8(map, "MessageB64"),
          UserDetail = DecodeBase64Utf8(map, "UserDetailB64"),
          LogPath = DecodeBase64Utf8(map, "LogPathB64"),
          ComponentFailures = failures
        };
      } catch {
        return null;
      }
    }

    private static string DecodeBase64Utf8(IDictionary<string, string> values, string key) {
      if (values == null || string.IsNullOrWhiteSpace(key) || !values.ContainsKey(key)) {
        return string.Empty;
      }

      var raw = values[key] ?? string.Empty;
      if (raw.Length == 0) {
        return string.Empty;
      }

      try {
        return Encoding.UTF8.GetString(Convert.FromBase64String(raw));
      } catch {
        return string.Empty;
      }
    }

    private static void TryDeleteFile(string path) {
      if (string.IsNullOrWhiteSpace(path)) {
        return;
      }
      try {
        if (File.Exists(path)) {
          File.Delete(path);
        }
      } catch {
      }
    }

    private static int RunElevatedBootstrapper(IReadOnlyList<string> arguments) {
      var executablePath = Assembly.GetExecutingAssembly().Location;
      var startInfo = new ProcessStartInfo {
        FileName = executablePath,
        Arguments = BuildCommandLine(arguments),
        UseShellExecute = true,
        Verb = "runas",
        WorkingDirectory = AppDomain.CurrentDomain.BaseDirectory
      };

      try {
        using (var process = Process.Start(startInfo)) {
          if (process == null) {
            return 1;
          }
          process.WaitForExit();
          return process.ExitCode;
        }
      } catch (Win32Exception ex) {
        if (ex.NativeErrorCode == 1223) {
          return 1223;
        }
        throw;
      }
    }

    private static int RunMsiexec(IReadOnlyList<string> arguments, bool hiddenWindow, bool requestElevationIfNeeded) {
      var shouldElevate = requestElevationIfNeeded && !IsProcessElevated();
      var startInfo = new ProcessStartInfo {
        FileName = ResolveMsiexecPath(),
        Arguments = BuildCommandLine(arguments),
        UseShellExecute = shouldElevate,
        WorkingDirectory = AppDomain.CurrentDomain.BaseDirectory
      };

      if (shouldElevate) {
        startInfo.Verb = "runas";
      } else {
        startInfo.CreateNoWindow = hiddenWindow;
      }

      try {
        using (var process = Process.Start(startInfo)) {
          if (process == null) {
            return 1;
          }
          process.WaitForExit();
          return process.ExitCode;
        }
      } catch (Win32Exception ex) {
        if (ex.NativeErrorCode == 1223) {
          return 1223;
        }
        throw;
      }
    }

    private static string ResolveMsiexecPath() {
      var windowsDirectory = Environment.GetFolderPath(Environment.SpecialFolder.Windows);
      if (Environment.Is64BitOperatingSystem && !Environment.Is64BitProcess) {
        return Path.Combine(windowsDirectory, "Sysnative", "msiexec.exe");
      }
      return Path.Combine(windowsDirectory, "System32", "msiexec.exe");
    }

    private static string BuildCommandLine(IEnumerable<string> arguments) {
      return string.Join(" ", arguments.Select(QuoteArgument));
    }

    private static string QuoteArgument(string argument) {
      if (string.IsNullOrEmpty(argument)) {
        return "\"\"";
      }
      if (argument.Contains("\"")) {
        return argument;
      }
      if (argument.IndexOf(' ') >= 0 || argument.IndexOf('\t') >= 0) {
        return "\"" + argument + "\"";
      }
      return argument;
    }

    private static string CreatePropertyArgument(string propertyName, string propertyValue) {
      var escaped = propertyValue.Replace("\"", "\"\"");
      return propertyName + "=\"" + escaped + "\"";
    }

    private static bool IsProcessElevated() {
      using (var identity = WindowsIdentity.GetCurrent()) {
        var principal = new WindowsPrincipal(identity);
        return principal.IsInRole(WindowsBuiltInRole.Administrator);
      }
    }

    private static string BuildResultMessage(string operationName, int exitCode, string logPath) {
      if (exitCode == 0 || exitCode == 3010) {
        return operationName + " succeeded.";
      }
      if (exitCode == 1223) {
        return operationName + " cancelled.";
      }

      var message = operationName + " failed (error " + exitCode + ").";
      if (exitCode == 1603) {
        message += " A fatal error occurred during installation. Ensure no Vibepollo processes are running and try again.";
      } else if (exitCode == 1618) {
        message += " Another installation is already in progress. Wait for it to finish, then try again.";
      } else if (exitCode == 1602) {
        message += " The installation was cancelled by the user.";
      } else if (exitCode == 1605) {
        message += " No existing Vibepollo installation was found.";
      }
      if (!string.IsNullOrWhiteSpace(logPath)) {
        message += " Log: " + logPath;
      }
      return message;
    }
  }

  internal static class ModernFolderPicker {
    private const uint FOS_PICKFOLDERS = 0x00000020;
    private const uint FOS_FORCEFILESYSTEM = 0x00000040;
    private const uint FOS_PATHMUSTEXIST = 0x00000800;
    private const uint SIGDN_FILESYSPATH = 0x80058000;

    [ComImport]
    [Guid("42F85136-DB7E-439C-85F1-E4075D135FC8")]
    [InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    private interface IFileDialog {
      [PreserveSig] int Show(IntPtr parent);
      void SetFileTypes(uint cFileTypes, IntPtr rgFilterSpec);
      void SetFileTypeIndex(uint iFileType);
      void GetFileTypeIndex(out uint piFileType);
      void Advise(IntPtr pfde, out uint pdwCookie);
      void Unadvise(uint dwCookie);
      void SetOptions(uint fos);
      void GetOptions(out uint pfos);
      void SetDefaultFolder(IShellItem psi);
      void SetFolder(IShellItem psi);
      void GetFolder(out IShellItem ppsi);
      void GetCurrentSelection(out IShellItem ppsi);
      void SetFileName([MarshalAs(UnmanagedType.LPWStr)] string pszName);
      void GetFileName([MarshalAs(UnmanagedType.LPWStr)] out string pszName);
      void SetTitle([MarshalAs(UnmanagedType.LPWStr)] string pszTitle);
      void SetOkButtonLabel([MarshalAs(UnmanagedType.LPWStr)] string pszText);
      void SetFileNameLabel([MarshalAs(UnmanagedType.LPWStr)] string pszLabel);
      void GetResult(out IShellItem ppsi);
      void AddPlace(IShellItem psi, int fdap);
      void SetDefaultExtension([MarshalAs(UnmanagedType.LPWStr)] string pszDefaultExtension);
      void Close(int hr);
      void SetClientGuid(ref Guid guid);
      void ClearClientData();
      void SetFilter(IntPtr pFilter);
    }

    [ComImport]
    [Guid("D57C7288-D4AD-4768-BE02-9D969532D960")]
    [InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    private interface IFileOpenDialog : IFileDialog {
      void GetResults(out IntPtr ppenum);
      void GetSelectedItems(out IntPtr ppsai);
    }

    [ComImport]
    [Guid("43826D1E-E718-42EE-BC55-A1E261C37BFE")]
    [InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    private interface IShellItem {
      void BindToHandler(IntPtr pbc, ref Guid bhid, ref Guid riid, out IntPtr ppv);
      void GetParent(out IShellItem ppsi);
      void GetDisplayName(uint sigdnName, out IntPtr ppszName);
      void GetAttributes(uint sfgaoMask, out uint psfgaoAttribs);
      void Compare(IShellItem psi, uint hint, out int piOrder);
    }

    [DllImport("shell32.dll", CharSet = CharSet.Unicode, PreserveSig = true)]
    private static extern int SHCreateItemFromParsingName([MarshalAs(UnmanagedType.LPWStr)] string pszPath, IntPtr pbc, [In] ref Guid riid, [Out] out IShellItem ppv);

    public static string TryPickFolder(Window owner, string title, string initialPath) {
      object dialogComObject = null;
      try {
        var dialogType = Type.GetTypeFromCLSID(new Guid("DC1C5A9C-E88A-4DDE-A5A1-60F82A20AEF7"));
        if (dialogType == null) {
          return null;
        }

        dialogComObject = Activator.CreateInstance(dialogType);
        var dialog = (IFileOpenDialog)dialogComObject;

        uint options;
        dialog.GetOptions(out options);
        options |= FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST;
        dialog.SetOptions(options);

        if (!string.IsNullOrWhiteSpace(title)) {
          dialog.SetTitle(title);
        }

        var normalizedInitial = NormalizeExistingFolder(initialPath);
        if (!string.IsNullOrWhiteSpace(normalizedInitial)) {
          var iidShellItem = new Guid("43826D1E-E718-42EE-BC55-A1E261C37BFE");
          IShellItem folderItem;
          var hrFolder = SHCreateItemFromParsingName(normalizedInitial, IntPtr.Zero, ref iidShellItem, out folderItem);
          if (hrFolder == 0 && folderItem != null) {
            dialog.SetFolder(folderItem);
          }
        }

        var ownerHandle = owner == null ? IntPtr.Zero : new WindowInteropHelper(owner).Handle;
        var hr = dialog.Show(ownerHandle);
        if (hr != 0) {
          return null;
        }

        IShellItem result;
        dialog.GetResult(out result);
        if (result == null) {
          return null;
        }

        IntPtr pathPtr;
        result.GetDisplayName(SIGDN_FILESYSPATH, out pathPtr);
        if (pathPtr == IntPtr.Zero) {
          return null;
        }

        try {
          return Marshal.PtrToStringUni(pathPtr);
        } finally {
          Marshal.FreeCoTaskMem(pathPtr);
        }
      } catch {
        return null;
      } finally {
        if (dialogComObject != null) {
          try {
            Marshal.FinalReleaseComObject(dialogComObject);
          } catch {
          }
        }
      }
    }

    private static string NormalizeExistingFolder(string path) {
      if (string.IsNullOrWhiteSpace(path)) {
        return null;
      }
      try {
        var full = Path.GetFullPath(path);
        if (Directory.Exists(full)) {
          return full;
        }
        var parent = Path.GetDirectoryName(full);
        if (!string.IsNullOrWhiteSpace(parent) && Directory.Exists(parent)) {
          return parent;
        }
      } catch {
      }
      return null;
    }
  }
}
