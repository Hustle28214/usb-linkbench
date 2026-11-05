# 贡献

提交到本仓库的代码和文档按 MIT OR Apache-2.0 授权。

改完请在本地运行：

```sh
cargo fmt --check
cargo clippy --all-targets -- -D warnings
cargo test
```

测量结果必须能指明种类：协商链路、描述符上限、控制回传、视频载荷、直接读。不要把其中一种填进另一种的字段。做不到的平台写成跳过，并说明原因。
