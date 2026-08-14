import type { Lang } from "./LanguageContext";

// Short chrome strings reused across many components (nav, footer, status badges, table
// headers, sidebar/TOC labels) — centralized here so "Real & tested" etc. gets translated
// once instead of re-typed at every one of its ~10 call sites.
export interface UiStrings {
  navHome: string;
  navPillars: string;
  navArchitecture: string;
  navSpecDriven: string;
  navGettingStarted: string;
  navApi: string;
  navGithub: string;
  langToggleLabel: string;
  footerRepository: string;
  footerSpecification: string;
  footerRfcIndex: string;
  breadcrumbApi: string;
  sidebarApiReference: string;
  sidebarOverview: string;
  tocOnThisPage: string;
  statusRealTested: string;
  statusDesignedNotBuilt: string;
  statusReal: string;
  statusDesign: string;
  required: string;
  optional: string;
}

export const ui: Record<Lang, UiStrings> = {
  en: {
    navHome: "AgentEngine home",
    navPillars: "Pillars",
    navArchitecture: "Architecture",
    navSpecDriven: "Spec-driven",
    navGettingStarted: "Getting started",
    navApi: "API",
    navGithub: "GitHub",
    langToggleLabel: "Switch language",
    footerRepository: "Repository",
    footerSpecification: "Specification",
    footerRfcIndex: "RFC index",
    breadcrumbApi: "API",
    sidebarApiReference: "API reference",
    sidebarOverview: "Overview",
    tocOnThisPage: "On this page",
    statusRealTested: "Real & tested",
    statusDesignedNotBuilt: "Designed, not built",
    statusReal: "real",
    statusDesign: "design",
    required: "required",
    optional: "optional",
  },
  vi: {
    navHome: "Trang chủ AgentEngine",
    navPillars: "Trụ cột",
    navArchitecture: "Kiến trúc",
    navSpecDriven: "Hướng đặc tả",
    navGettingStarted: "Bắt đầu",
    navApi: "API",
    navGithub: "GitHub",
    langToggleLabel: "Đổi ngôn ngữ",
    footerRepository: "Kho mã nguồn",
    footerSpecification: "Đặc tả kỹ thuật",
    footerRfcIndex: "Danh mục RFC",
    breadcrumbApi: "API",
    sidebarApiReference: "Tài liệu API",
    sidebarOverview: "Tổng quan",
    tocOnThisPage: "Trong trang này",
    statusRealTested: "Đã có thật & kiểm thử",
    statusDesignedNotBuilt: "Mới thiết kế, chưa xây",
    statusReal: "đã có",
    statusDesign: "thiết kế",
    required: "bắt buộc",
    optional: "tùy chọn",
  },
};
