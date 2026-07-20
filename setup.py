from setuptools import find_packages, setup


setup(
    name="netft_driver",
    version="0.1.0",
    packages=find_packages(exclude=("test", "test.*")),
    python_requires=">=3.8",
    install_requires=[],
    zip_safe=True,
    license="MIT",
    description="ROS 1 and ROS 2 driver for ATI Net F/T sensors",
    maintainer="Xudong Han",
    maintainer_email="hanxudong159@126.com",
    url="https://github.com/han-xudong/ros-netft",
    project_urls={
        "Bug Tracker": "https://github.com/han-xudong/ros-netft/issues",
    },
)
